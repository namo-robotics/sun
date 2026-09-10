import { readFileSync } from 'node:fs'
import { getHighlighter, BUNDLED_LANGUAGES, renderToHtml } from 'shiki'

/** Highlights code with the extension's grammar and VS Code's light and dark palettes. */
export async function createHighlighter(options) {
  const grammar = JSON.parse(readFileSync(
    new URL('../../extensions/vscode-sun/syntaxes/sun.tmLanguage.json', import.meta.url),
    'utf8'
  ))
  const highlighter = await getHighlighter({
    ...options,
    theme: 'dark-plus',
    langs: [...BUNDLED_LANGUAGES, {
      id: 'sun', scopeName: 'source.sun', aliases: ['moon'], path: '', grammar,
    }],
  })
  await highlighter.loadTheme('light-plus')

  // Nextra keeps only the first code block from a multiple-theme result.
  // Store both palettes on each span so switching themes keeps one copy.
  return {
    ...highlighter,
    codeToHtml(code, lang) {
      const dark = highlighter.codeToThemedTokens(code, lang, 'dark-plus')
      const light = highlighter.codeToThemedTokens(code, lang, 'light-plus')
      const lines = dark.map((line, lineIndex) => {
        const output = []
        let lightIndex = 0
        let lightOffset = 0
        for (const token of line) {
          let offset = 0
          while (offset < token.content.length) {
            const other = light[lineIndex][lightIndex]
            const length = Math.min(token.content.length - offset, other.content.length - lightOffset)
            output.push({ ...token, content: token.content.slice(offset, offset + length), light: other })
            offset += length
            lightOffset += length
            if (lightOffset === other.content.length) {
              lightIndex++
              lightOffset = 0
            }
          }
        }
        return output
      })
      return renderToHtml(lines, {
        elements: {
          token: ({ token, children }) => {
            const styles = [['dark', token], ['light', token.light]].flatMap(([mode, value]) => [
              `--sun-${mode}-color:${value.color || highlighter.getForegroundColor(`${mode}-plus`)}`,
              `--sun-${mode}-style:${value.fontStyle & 1 ? 'italic' : 'normal'}`,
              `--sun-${mode}-weight:${value.fontStyle & 2 ? 'bold' : 'normal'}`,
              `--sun-${mode}-decoration:${value.fontStyle & 4 ? 'underline' : 'none'}`,
            ])
            return `<span class="sun-code-token" style="${styles.join(';')}">${children}</span>`
          },
        },
      })
    },
  }
}
