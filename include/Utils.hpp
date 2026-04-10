/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#include <syslog.h>
#include <spawn.h>
#include <fcntl.h>

extern char **environ;

namespace ucc
{

// ---------------------------------------------------------------------------
// Device support policy
//
// UCC is allowed to start on any Linux system. Hardware-specific features still
// rely on the usual driver/sysfs capability checks before doing useful work.
// ---------------------------------------------------------------------------

/**
 * @brief Check whether UCC should start on the current machine.
 */
inline bool isDeviceSupported()
{
  return true;
}

/**
 * @brief Execute a process safely with an argument vector (no shell).
 *
 * This replaces the old popen()-based executeCommand() to prevent
 * shell injection attacks.  The executable is looked up via PATH
 * by posix_spawnp().
 *
 * @param executable Path or name of the executable
 * @param args       Argument vector (argv[0] is set to executable automatically)
 * @param envOverrides Optional environment variable overrides ("KEY=VALUE").
 *                     When non-empty, they are prepended to the inherited environ.
 * @return stdout of the child process, or "" on error
 */
[[nodiscard]] inline std::string executeProcess(
    const std::string &executable,
    const std::vector< std::string > &args,
    const std::vector< std::string > &envOverrides = {} ) noexcept
{
  try
  {
    // Build argv (must be null-terminated array of char*)
    std::vector< const char * > argv;
    argv.push_back( executable.c_str() );
    for ( const auto &arg : args )
      argv.push_back( arg.c_str() );
    argv.push_back( nullptr );

    // Build environment: overrides + inherited
    std::vector< std::string > envStorage;
    std::vector< const char * > envp;
    if ( not envOverrides.empty() )
    {
      for ( const auto &ov : envOverrides )
        envStorage.push_back( ov );
      // Copy current environment (skip keys that are overridden)
      std::vector< std::string > overrideKeys;
      for ( const auto &ov : envOverrides )
      {
        auto eq = ov.find( '=' );
        if ( eq != std::string::npos )
          overrideKeys.push_back( ov.substr( 0, eq + 1 ) ); // "KEY="
      }
      if ( environ )
      {
        for ( char **e = environ; *e; ++e )
        {
          std::string entry( *e );
          bool overridden = false;
          for ( const auto &key : overrideKeys )
          {
            if ( entry.substr( 0, key.size() ) == key )
            {
              overridden = true;
              break;
            }
          }
          if ( not overridden )
            envStorage.push_back( entry );
        }
      }
      for ( const auto &s : envStorage )
        envp.push_back( s.c_str() );
      envp.push_back( nullptr );
    }

    // Create pipe for stdout capture
    int pipeFds[2];
    if ( pipe( pipeFds ) != 0 )
      return "";

    // Set up posix_spawn file actions: child stdout → write end of pipe
    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_init( &fileActions );
    posix_spawn_file_actions_addclose( &fileActions, pipeFds[0] );                // close read end in child
    posix_spawn_file_actions_adddup2( &fileActions, pipeFds[1], STDOUT_FILENO );  // stdout → pipe write
    posix_spawn_file_actions_addclose( &fileActions, pipeFds[1] );                // close original write fd
    // Redirect stderr to /dev/null
    posix_spawn_file_actions_addopen( &fileActions, STDERR_FILENO, "/dev/null", O_WRONLY, 0 );

    pid_t pid = -1;
    int spawnResult = 0;
    if ( envOverrides.empty() )
    {
      spawnResult = posix_spawnp(
          &pid, executable.c_str(), &fileActions, nullptr,
          const_cast< char *const * >( argv.data() ), environ );
    }
    else
    {
      spawnResult = posix_spawnp(
          &pid, executable.c_str(), &fileActions, nullptr,
          const_cast< char *const * >( argv.data() ),
          const_cast< char *const * >( envp.data() ) );
    }

    posix_spawn_file_actions_destroy( &fileActions );

    // Close write end in parent
    close( pipeFds[1] );

    if ( spawnResult != 0 )
    {
      close( pipeFds[0] );
      return "";
    }

    // Read child stdout
    std::string result;
    char buffer[512];
    ssize_t bytesRead;
    while ( ( bytesRead = read( pipeFds[0], buffer, sizeof( buffer ) - 1 ) ) > 0 )
    {
      buffer[bytesRead] = '\0';
      result += buffer;
    }
    close( pipeFds[0] );

    // Wait for child
    int status = 0;
    waitpid( pid, &status, 0 );

    return result;
  }
  catch ( const std::exception & )
  {
    return "";
  }
}

/**
 * @brief Execute a shell command and capture output.
 *
 * @deprecated Use executeProcess() with explicit argument vectors instead.
 *             This wrapper exists only for commands that genuinely need
 *             shell features (glob expansion, pipes). All new code should
 *             use executeProcess().
 *
 * @param command Shell command string
 * @return Command output or empty string on error
 */
[[deprecated( "Use ucc::executeProcess() with argument vectors" )]]
[[nodiscard]] inline std::string executeCommand( const std::string &command ) noexcept
{
  return executeProcess( "/bin/sh", { "-c", command } );
}

/**
 * @brief Get list of device names in a directory
 * @param sourceDir Directory to list
 * @return Vector of device/file names
 */
[[nodiscard]] inline std::vector< std::string > getDeviceList( const std::string &sourceDir ) noexcept
{
  std::vector< std::string > devices;

  try
  {
    if ( !std::filesystem::exists( sourceDir ) || !std::filesystem::is_directory( sourceDir ) )
    {
      return devices;
    }

    for ( const auto &entry : std::filesystem::directory_iterator( sourceDir ) )
    {
      if ( entry.is_directory() || entry.is_symlink() || entry.is_regular_file() )
      {
        devices.push_back( entry.path().filename().string() );
      }
    }
  }
  catch ( const std::exception & )
  {
    // Return empty vector on error
  }

  return devices;
}

} // namespace ucc
