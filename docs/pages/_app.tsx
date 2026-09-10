import type { AppProps } from 'next/app'
import '../styles/code.css'

/** Applies shared code highlighting styles to the documentation pages. */
export default function App({ Component, pageProps }: AppProps) {
  return <Component {...pageProps} />
}
