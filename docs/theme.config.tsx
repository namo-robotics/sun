import React from 'react'
import { DocsThemeConfig } from 'nextra-theme-docs'

const config: DocsThemeConfig = {
  logo: <span style={{ fontWeight: 700 }}>☀️ Sun Language</span>,
  project: {
    link: 'https://github.com/namo-robotics/sun',
  },
  docsRepositoryBase: 'https://github.com/namo-robotics/sun/tree/main/docs',
  footer: {
    text: 'Sun Programming Language Documentation',
  },
  primaryHue: 45,
  primarySaturation: 100,
  useNextSeoProps() {
    return {
      titleTemplate: '%s – Sun Language'
    }
  },
  head: (
    <>
      <meta name="viewport" content="width=device-width, initial-scale=1.0" />
      <meta name="description" content="Sun - A fast, safe programming language with built-in matrix operations" />
      <meta name="og:title" content="Sun Programming Language" />
    </>
  ),
  sidebar: {
    defaultMenuCollapseLevel: 1,
    toggleButton: true,
  },
  toc: {
    backToTop: true,
  },
  darkMode: true,
  nextThemes: {
    defaultTheme: 'dark',
  },
}

export default config
