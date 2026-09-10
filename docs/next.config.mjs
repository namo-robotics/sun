import nextra from 'nextra'
import { createHighlighter } from './scripts/highlighter.mjs'

const withNextra = nextra({
  theme: 'nextra-theme-docs',
  themeConfig: './theme.config.tsx',
  mdxOptions: {
    rehypePrettyCodeOptions: {
      getHighlighter: createHighlighter,
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
