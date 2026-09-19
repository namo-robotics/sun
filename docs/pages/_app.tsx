import type { AppProps } from 'next/app'
import '../styles/code.css'
import '../styles/compiler-api.css'

/** Applies shared documentation styles to the documentation pages. */
export default function App({ Component, pageProps }: AppProps) {
  return <Component {...pageProps} />
}
