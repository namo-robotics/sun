import Link from 'next/link'
import { flushSync } from 'react-dom'
import { useEffect, useRef, useState, type ComponentProps, type MouseEvent } from 'react'

/** Animates native disclosure controls without changing their keyboard behavior. */
export function CompilerTreeBranch({ onBeforeExpand, ...props }: ComponentProps<'details'> & { onBeforeExpand?: () => void }) {
  const branch = useRef<HTMLDetailsElement>(null)
  const animation = useRef<Animation | null>(null)
  const opening = useRef(false)

  useEffect(() => () => animation.current?.cancel(), [])

  function toggle(event: MouseEvent<HTMLDetailsElement>) {
    const element = branch.current
    const target = event.target as HTMLElement
    const summary = element?.querySelector(':scope > summary') as HTMLElement | null
    if (!element || !summary || target.closest('summary') !== summary || target.closest('a')) return

    event.preventDefault()
    const expand = animation.current ? !opening.current : !element.open
    const start = element.getBoundingClientRect().height
    animation.current?.cancel()
    animation.current = null
    opening.current = expand
    if (expand) onBeforeExpand?.()

    if (window.matchMedia('(prefers-reduced-motion: reduce)').matches) {
      element.open = expand
      element.style.overflow = ''
      return
    }

    element.open = true
    const style = getComputedStyle(element)
    const frame = ['paddingTop', 'paddingBottom', 'borderTopWidth', 'borderBottomWidth']
      .reduce((height, property) => height + parseFloat(style[property as keyof CSSStyleDeclaration] as string), 0)
    const end = expand ? element.getBoundingClientRect().height : summary.getBoundingClientRect().height + frame
    element.style.overflow = 'hidden'
    const transition = element.animate([{ height: `${start}px` }, { height: `${end}px` }], {
      duration: 180,
      easing: 'ease-out',
    })
    animation.current = transition
    transition.onfinish = () => {
      element.open = expand
      element.style.overflow = ''
      animation.current = null
    }
  }

  return <details {...props} ref={branch} onClick={toggle} />
}

/** Keeps namespace links and disclosure controls independently usable. */
export function CompilerTreeSummary(props: ComponentProps<'summary'>) {
  return <summary {...props} />
}

type ApiTreeNode = {
  label: string
  href?: string
  kind?: string
  children: ApiTreeNode[]
}

function ApiTreeBranch({ node, depth }: { node: ApiTreeNode; depth: number }) {
  const initiallyOpen = depth === 0 && node.label === 'sun'
  const [mounted, setMounted] = useState(initiallyOpen)
  const label = <>
    {node.href ? <Link href={node.href}>{node.label}</Link> : node.label}
    {node.kind === 'namespace' && <span className="compiler-tree-kind">namespace</span>}
  </>
  if (!node.children.length) return <div className="compiler-namespace-leaf">{label}</div>
  return <CompilerTreeBranch open={initiallyOpen} onBeforeExpand={() => {
    if (!mounted) flushSync(() => setMounted(true))
  }}>
    <CompilerTreeSummary>{label}</CompilerTreeSummary>
    <div className="compiler-namespace-children">{mounted && node.children.map(child =>
      <ApiTreeBranch key={child.href || child.label} node={child} depth={depth + 1} />
    )}</div>
  </CompilerTreeBranch>
}

/** Shows the combined reference tree, rendering branch contents when expanded. */
export function CompilerApiTree({ nodes }: { nodes: ApiTreeNode[] }) {
  return <div className="compiler-namespace-tree">{nodes.map(node =>
    <ApiTreeBranch key={node.href || node.label} node={node} depth={0} />
  )}</div>
}
