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
  // Development and production builds can run at the same time.
  distDir: process.env.NODE_ENV === 'production' ? '.next' : '.next-dev',
  // Keep the compiler reference build within a bounded number of workers.
  experimental: { webpackBuildWorker: true, cpus: 2 },
  /** Disables production build caching to limit memory used by generated navigation. */
  webpack(config, { dev }) {
    // Nextra embeds generated navigation in every page.
    if (!dev) config.cache = false
    return config
  },
  images: {
    unoptimized: true,
  },
  basePath: process.env.NODE_ENV === 'production' ? '/sun' : '',
  trailingSlash: true,
})
