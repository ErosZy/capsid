import assert from 'node:assert/strict';
import test from 'node:test';
import * as prettier from 'prettier';
import { assertEquivalentJS } from '../tools/format-js.mjs';

test('actual Prettier output preserves BigInt, regexp, templates, directives and async modules', async () => {
  const source = `'use strict'; const values=[0xffffffffffffffffn,/abc/giu,Infinity]; export async function f(x){return x?.value??values[0]}`;
  const output = await prettier.format(source, { parser: 'babel', singleQuote: true });
  assert.notEqual(output, source);
  assertEquivalentJS(source, output);
});

test('AST comparison rejects real value and operator changes', () => {
  for (const [before, after] of [
    ['const value=9007199254740993n;', 'const value=9007199254740992n;'],
    ['export const f=(a,b)=>a+b;', 'export const f=(a,b)=>a-b;'],
    ['const f=()=>/abc/u;', 'const f=()=>/abd/u;'],
    ['const f=()=>/abc/u;', 'const f=()=>/abc/ui;'],
    ['const f=()=>tag`\\n`;', 'const f=()=>tag`\n`;'],
  ]) {
    assert.throws(() => assertEquivalentJS(before, after), /formatting changed/);
  }
});

test('regular-expression flag order does not change its meaning', () => {
  assertEquivalentJS('const value=/abc/mi;', 'const value = /abc/im;');
});

test('malformed formatted output cannot pass comparison', () => {
  assert.throws(() => assertEquivalentJS('const a = 1;', 'const = ;'), SyntaxError);
});
