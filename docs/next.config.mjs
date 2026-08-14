import nextra from 'nextra'
import { readFileSync } from 'node:fs'
import { getHighlighter, BUNDLED_LANGUAGES } from 'shiki'

// Register the Sun TextMate grammar (shared with the VS Code extension) so
// ```sun code fences get syntax highlighting.
const sunGrammar = JSON.parse(
  readFileSync(
    new URL('../extensions/vscode-sun/syntaxes/sun.tmLanguage.json', import.meta.url),
    'utf8'
  )
)

const withNextra = nextra({
  theme: 'nextra-theme-docs',
  themeConfig: './theme.config.tsx',
  mdxOptions: {
    rehypePrettyCodeOptions: {
      getHighlighter: options =>
        getHighlighter({
          ...options,
          langs: [
            ...BUNDLED_LANGUAGES,
            {
              id: 'sun',
              scopeName: 'source.sun',
              aliases: ['moon'],
              path: '',
              grammar: sunGrammar,
            },
          ],
        }),
    },
  },
})

export default withNextra({
  output: 'export',
  images: {
    unoptimized: true,
  },
  basePath: process.env.NODE_ENV === 'production' ? '/sun' : '',
  trailingSlash: true,
})
