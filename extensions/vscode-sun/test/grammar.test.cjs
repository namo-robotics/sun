/** Verifies exception tokens in the grammar used before semantic analysis completes. */
const assert = require('node:assert/strict');
const { test } = require('node:test');
const grammar = require('../syntaxes/sun.tmLanguage.json');

/** Ensures exception control flow and declarations receive the intended scopes. */
test('exception syntax is highlighted', () => {
  for (const [token, scope] of [
    ['throw', 'keyword.control.sun'], ['catch', 'keyword.control.sun'],
    ['throws', 'storage.modifier.sun'], ['exception', 'storage.type.sun'],
  ]) {
    assert.ok(grammar.repository.keywords.patterns.some(
      pattern => pattern.name === scope && new RegExp(pattern.match).test(token)));
  }
  assert.ok(grammar.repository.declarations.patterns.some(
    pattern => new RegExp(pattern.match).test('exception DeviceFailure')));
});
