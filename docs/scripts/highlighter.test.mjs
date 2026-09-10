import assert from 'node:assert/strict'
import test from 'node:test'
import { createHighlighter } from './highlighter.mjs'

const highlighter = await createHighlighter({})

for (const [lang, source] of [
  ['sun', 'public function read(x: u32) bool { return x < 0x71u32 and true; }'],
  ['sun', 'class Packet { init() {} }\nvar bits = 0b1000_0001u8;\nvar n = 1_000;'],
  ['sun', '/* comment */\n\nvar text = "<&\\\"";\n'],
  ['sun', '`hello ${read(value)}`'],
  ['sun', ''],
  ['cpp', 'class Packet { public: int read() { return 42; } };'],
  ['markdown', '# Heading\n**bold** and *italic*'],
  ['text', 'plain <text> & spaces'],
]) {
  test(`${lang}: both palettes preserve the theme colors and source text`, () => {
    const html = highlighter.codeToHtml(source, lang)
    const spans = [...html.matchAll(/<span class="sun-code-token" style="([^"]*)">([^<]*)<\/span>/g)]
    for (const mode of ['dark', 'light']) {
      const expected = highlighter.codeToThemedTokens(source, lang, `${mode}-plus`)
        .flatMap(line => line.flatMap(token => [...token.content].map(char => [
          char, token.color || highlighter.getForegroundColor(`${mode}-plus`),
          token.fontStyle & 1 ? 'italic' : 'normal',
          token.fontStyle & 2 ? 'bold' : 'normal',
          token.fontStyle & 4 ? 'underline' : 'none',
        ])))
      const actual = spans.flatMap(([, style, content]) => {
        const styles = Object.fromEntries(style.split(';').map(entry => entry.split(':')))
        const decoded = content.replace(/&(amp|lt|gt|quot|#39);/g,
          entity => ({ '&amp;': '&', '&lt;': '<', '&gt;': '>', '&quot;': '"', '&#39;': "'" })[entity])
        return [...decoded].map(char => [char, ...['color', 'style', 'weight', 'decoration']
          .map(property => styles[`--sun-${mode}-${property}`])])
      })
      assert.deepEqual(actual, expected)
    }
  })
}
