/*
   Host test for the web interface's RTTTL parser (www/index.html, parseMelody): it reads a melody as the panel
   does (sound/Sound.h melody_notes). test/host/sound_test.cpp writes what the panel's parser makes of a set of
   cases to test/host/build/melody_cases.json; here the web interface's parser must make the same of each: the
   same notes, or the same error. Run by test/host/run.sh after sound_test.
*/

const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync('www/index.html', 'utf8');
const pick = (re, what) => {
  const m = html.match(re);
  if (!m) { console.error('FAIL melody: no ' + what + ' in www/index.html'); process.exit(1); }
  return m[0];
};
const code = pick(/function parseMelody\([\s\S]*?\n}\n/, 'parseMelody') + pick(/const MAX_MELODY_NOTES = \d+;/, 'MAX_MELODY_NOTES');
const box = {};
vm.runInNewContext(code + '\nthis.parseMelody = parseMelody;', box);

const cases = JSON.parse(fs.readFileSync('test/host/build/melody_cases.json', 'utf8'));
// too many notes, built here rather than written out
cases.push({in: 'x::' + 'a,'.repeat(401), ok: false, error: 'more than 400 notes'});
let failures = 0;
for (const c of cases) {
  const p = box.parseMelody(c.in);
  const got = p.error ? {ok: false, error: p.error} : {ok: true, notes: p.notes.map(n => [n.hz, n.ms])};
  const want = c.ok ? {ok: true, notes: c.notes} : {ok: false, error: c.error};
  if (JSON.stringify(got) !== JSON.stringify(want)) {
    console.error('FAIL melody ' + JSON.stringify(c.in) + '\n  web:   ' + JSON.stringify(got) + '\n  panel: ' + JSON.stringify(want));
    failures++;
  }
}
// a note's place in the text, to mark it: "x" in "x:d=4: a, x"
const p = box.parseMelody('x:d=4: a, x');
if (p.at !== 10 || p.len !== 1) { console.error('FAIL melody: the wrong note is at ' + p.at + '+' + p.len); failures++; }
if (failures) { console.error(failures + ' melody check(s) failed'); process.exit(1); }
console.log('melody: the web interface reads all ' + cases.length + ' cases as the panel does');
