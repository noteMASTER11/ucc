// Exercise the actual tray button callback against the GNOME app boundary.
const fs=require('node:fs');
const vm=require('node:vm');
const assert=require('node:assert/strict');
const source=fs.readFileSync(`${__dirname}/../ucc-gnome/extension.js`,'utf8');
const body=source.match(/openBtn\.connect\('clicked', \(\) => \{([\s\S]*?)\n        \}\);/)[1];
let launches=0,closed=0;
const window={minimized:true,focused:false};
const shellApp={activate(){window.minimized=false;window.focused=true;}};
const context={Shell:{AppSystem:{get_default:()=>({lookup_app:id=>{
 assert.equal(id,'ucc-gui.desktop');return shellApp;
}})}},GLib:{spawn_command_line_async:()=>{launches++;}},owner:{menu:{close(){closed++;}}}};
vm.runInNewContext(`(function(){${body}}).call(owner)`,context);
assert.equal(launches,0,'Existing window must be activated instead of spawning another process');
assert.equal(window.minimized,false);
assert.equal(window.focused,true);
assert.equal(closed,1);
context.Shell.AppSystem.get_default=()=>({lookup_app:()=>null});
vm.runInNewContext(`(function(){${body}}).call(owner)`,context);
assert.equal(launches,1,'Fallback still opens the singleton GUI when the desktop entry is unavailable');
console.log('PASS: tray reuses the GNOME app and closes its menu');
