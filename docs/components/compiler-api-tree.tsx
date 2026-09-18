import type { ComponentProps } from 'react'

/** Keeps compiler tree branches as native disclosure controls. */
export function CompilerTreeBranch(props: ComponentProps<'details'>) {
  return <details {...props} />
}

/** Keeps namespace links and disclosure controls independently usable. */
export function CompilerTreeSummary(props: ComponentProps<'summary'>) {
  return <summary {...props} />
}
