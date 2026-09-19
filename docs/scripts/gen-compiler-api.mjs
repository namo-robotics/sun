/** Generates the compiler reference from Doxygen XML and source comments. */
import { DOMParser } from '@xmldom/xmldom'
import { execFileSync } from 'node:child_process'
import { readFileSync, writeFileSync, mkdirSync, readdirSync, rmSync } from 'node:fs'
import { dirname, join, resolve, relative, isAbsolute } from 'node:path'
import { fileURLToPath } from 'node:url'

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '../..')
const VERSION = '1.15.0'
const ROUTE = '/compiler-api'
const kinds = new Set(['namespace', 'class', 'struct', 'union', 'file'])
const children = (node, tag) => Array.from(node?.childNodes || []).filter(n => n.nodeType === 1 && (!tag || n.tagName === tag))
const child = (node, tag) => children(node, tag)[0]
const value = (node, tag) => child(node, tag)?.textContent || ''
const attr = (node, key) => node?.getAttribute(key) || ''
const descendants = (node, tag) => Array.from(node.getElementsByTagName(tag))
const compare = (a, b) => a < b ? -1 : a > b ? 1 : 0
const visibleName = name => name.replace(/anonymous_namespace\{[^}]*\}::?/g, '').replace(/::$/, '')
const compoundName = node => attr(node, 'kind') === 'file' ? attr(child(node, 'location'), 'file') || value(node, 'compoundname') : value(node, 'compoundname')

// Character references keep source text from becoming Markdown or executable MDX.
const escape = text => String(text).replace(/[&<>\{\}\\`*_\[\]#!|~]/g, c => `&#${c.codePointAt(0)};`).replace(/^(\s*)(import|export)\b/gm, (_, space, word) => `${space}&#${word.codePointAt(0)};${word.slice(1)}`)
const code = text => `<code>${escape(text)}</code>`
const fence = text => {
  const marker = '`'.repeat(Math.max(3, ...Array.from(text.matchAll(/`+/g), m => m[0].length + 1)))
  return `\n\n${marker}cpp\n${text}\n${marker}\n\n`
}
const link = (label, url) => `[${escape(label)}](${url})`
const safeId = id => {
  if (!/^[a-zA-Z0-9_-]+$/.test(id)) throw new Error(`Invalid Doxygen identifier: ${id}`)
  return id
}

/** Parses Doxygen XML, rejecting malformed documents. */
export function parseXml(xml) {
  return new DOMParser({ onError: (level, message) => { throw new Error(`Doxygen XML: ${message}`) } }).parseFromString(xml, 'application/xml')
}

function sourceLink(node, revision) {
  const location = child(node, 'location')
  if (!location) return ''
  const file = attr(location, 'file') || attr(location, 'bodyfile')
  const path = isAbsolute(file) ? relative(ROOT, file) : file
  if (!/^(include|src)\//.test(path) || path.split('/').includes('..')) return ''
  const line = attr(location, 'line') || attr(location, 'bodystart')
  return link('Source', `https://github.com/namo-robotics/sun/blob/${revision}/${path.split('/').map(encodeURIComponent).join('/')}${/^\d+$/.test(line) ? `#L${line}` : ''}`)
}

function renderComment(node, refs) {
  if (!node) return ''
  if (node.nodeType === 3 || node.nodeType === 4) return escape(node.data)
  const content = () => Array.from(node.childNodes || []).map(n => renderComment(n, refs)).join('')
  switch (node.tagName) {
    case 'ref': {
      const url = refs.get(attr(node, 'refid'))
      return url ? link(node.textContent, url) : escape(node.textContent)
    }
    case 'ulink': {
      const url = attr(node, 'url')
      return /^(https?:\/\/|mailto:)/.test(url) ? link(node.textContent, encodeURI(url).replace(/[()]/g, c => c === '(' ? '%28' : '%29')) : escape(node.textContent)
    }
    case 'para': return `${content().trim()}\n\n`
    case 'computeroutput': return code(node.textContent)
    case 'bold': return `<strong>${content()}</strong>`
    case 'emphasis': return `<em>${content()}</em>`
    case 'linebreak': return '<br />\n'
    case 'sp': return ' '
    case 'programlisting': return fence(children(node, 'codeline').map(line => {
      const plain = n => n.nodeType === 3 ? n.data : n.tagName === 'sp' ? ' ' : Array.from(n.childNodes || []).map(plain).join('')
      return plain(line)
    }).join('\n'))
    case 'verbatim': case 'preformatted': return fence(node.textContent)
    case 'itemizedlist': case 'orderedlist': return `\n${children(node, 'listitem').map((item, i) => `${node.tagName === 'orderedlist' ? `${i + 1}.` : '-'} ${renderComment(item, refs).trim().replace(/\n/g, '\n   ')}`).join('\n')}\n\n`
    case 'parameterlist': return `\n**${escape(attr(node, 'kind'))}**\n\n${content()}`
    case 'parameteritem': return `- ${descendants(node, 'parametername').map(n => code(n.textContent)).join(', ')}: ${renderComment(child(node, 'parameterdescription'), refs).trim().replace(/\n/g, '\n  ')}\n\n`
    case 'simplesect': return `\n**${escape(attr(node, 'kind'))}:** ${content()}\n\n`
    case 'title': return `\n**${content()}**\n\n`
    case 'table': return `\n<table>\n${content()}</table>\n\n`
    case 'row': return `<tr>${content()}</tr>\n`
    case 'entry': return `<td>${content().trim()}</td>`
    case 'htmlonly': case 'latexonly': case 'xmlonly': case 'docbookonly': case 'rtfonly': return ''
    default: return content()
  }
}

function description(node, refs) {
  const parts = ['briefdescription', 'detaileddescription'].map(tag => renderComment(child(node, tag), refs).trim()).filter(Boolean)
  return parts.length ? [...new Set(parts)].join('\n\n') : 'No documentation comment.'
}

function template(node) {
  const params = children(child(node, 'templateparamlist'), 'param')
  return params.length ? `template <${params.map(p => `${value(p, 'type')} ${value(p, 'declname')}${value(p, 'defval') ? ` = ${value(p, 'defval')}` : ''}`.trim()).join(', ')}>\n` : ''
}

function signature(node) {
  if (attr(node, 'kind') === 'enum') {
    return `enum${attr(node, 'strong') === 'yes' ? ' class' : ''} ${value(node, 'qualifiedname') || value(node, 'name')}${value(node, 'type') ? ` : ${value(node, 'type')}` : ''}`
  }
  const definition = value(node, 'definition') || `${value(node, 'type')} ${value(node, 'name')}`.trim()
  const initializer = value(node, 'initializer')
  return template(node) + definition + value(node, 'argsstring') + (initializer ? ` ${initializer}` : '')
}

const memberKinds = { function: 'Functions', variable: 'Fields', enum: 'Enums', typedef: 'Type Aliases', friend: 'Friends' }
const memberGroup = (member, kind) => {
  const access = attr(member, 'prot') || (kind === 'class' ? 'private' : 'public')
  return `${access[0].toUpperCase() + access.slice(1)} ${memberKinds[attr(member, 'kind')] || 'Other Members'}`
}

const namedSunNamespace = name => /^sun(?:::[A-Za-z_]\w*)*$/.test(name)

/** Groups symbols under named Sun namespaces, keeping file-local entries accessible. */
export function renderSymbolTree(title, entries, namespaces, refs) {
  const root = { children: new Map(), entries: [] }
  const known = new Map(namespaces.filter(n => namedSunNamespace(compoundName(n))).map(n => [compoundName(n), n]))
  const branch = name => {
    let node = root
    let path = ''
    for (const part of name.split('::')) {
      path = path ? `${path}::${part}` : part
      if (!node.children.has(part)) node.children.set(part, { name: path, children: new Map(), entries: [] })
      node = node.children.get(part)
    }
    return node
  }
  for (const [name, href, owner = '', file = ''] of entries) {
    // Anonymous scopes belong under their nearest named namespace.
    const parts = owner.split('::')
    while (parts.length && !namedSunNamespace(parts.join('::'))) parts.pop()
    const namespace = parts.join('::')
    let node = namespace ? branch(namespace) : branch('File scope')
    if (!namespace && file) {
      if (!node.children.has(file)) node.children.set(file, { name: file, children: new Map(), entries: [] })
      node = node.children.get(file)
    }
    const display = visibleName(name)
    node.entries.push([namespace && display.startsWith(`${namespace}::`) ? display.slice(namespace.length + 2) : display, href])
  }
  if (title === 'Namespaces') for (const name of known.keys()) branch(name)
  const render = (node, depth) => {
    const label = node.name.split('::').at(-1)
    const target = known.get(node.name)
    const heading = target ? `<NamespaceLink href="${refs.get(attr(target, 'id'))}">${escape(label)}</NamespaceLink>` : escape(label)
    const nested = [...node.children.values()].sort((a, b) => compare(a.name, b.name)).map(n => render(n, depth + 1))
    const symbols = node.entries.sort((a, b) => compare(a[0], b[0]) || compare(a[1], b[1])).map(([name, href]) => `<div className="compiler-namespace-leaf"><NamespaceLink href="${href}">${escape(name)}</NamespaceLink></div>`)
    if (!nested.length && !symbols.length) return `<div className="compiler-namespace-leaf">${heading}</div>`
    return `<TreeBranch${depth === 0 ? ' open' : ''}>\n<TreeSummary>${heading}</TreeSummary>\n<div className="compiler-namespace-children">\n${[...symbols, ...nested].join('\n')}\n</div>\n</TreeBranch>`
  }
  return `import NamespaceLink from 'next/link'\nimport { CompilerTreeBranch as TreeBranch, CompilerTreeSummary as TreeSummary } from '../../components/compiler-api-tree'\n\n# ${title}\n\n<div className="compiler-namespace-tree">\n${[...root.children.values()].sort((a, b) => compare(a.name, b.name)).map(n => render(n, 0)).join('\n')}\n</div>\n`
}

/** Renders named Sun namespaces as keyboard-accessible disclosure controls. */
export function renderNamespaceTree(namespaces, refs) {
  return renderSymbolTree('Namespaces', [], namespaces, refs)
}

/** Renders compound pages and indexes with stable links to compiler symbols. */
export function renderReference(compounds, revision) {
  if (!/^[a-f0-9]{40}$/.test(revision)) throw new Error('Source revision must be a full Git commit hash')
  compounds = compounds.filter(n => kinds.has(attr(n, 'kind'))).map(n => n.cloneNode(true)).sort((a, b) => compare(compoundName(a), compoundName(b)) || compare(attr(a, 'id'), attr(b, 'id')))
  // File-local declarations stay documented on their source file's page.
  const fileScopes = new Map()
  const files = new Map(compounds.filter(n => attr(n, 'kind') === 'file').map(n => [attr(child(n, 'location'), 'file') || compoundName(n), n]))
  for (const compound of compounds) {
    if (attr(compound, 'kind') !== 'namespace' || !compoundName(compound).includes('anonymous_namespace{')) continue
    const file = files.get(attr(child(compound, 'location'), 'file'))
    if (!file) throw new Error(`Missing source file for internal scope: ${compoundName(compound)}`)
    fileScopes.set(attr(compound, 'id'), attr(file, 'id'))
    for (const member of descendants(compound, 'memberdef')) {
      const copy = member.cloneNode(true)
      copy.setAttribute('data-sun-namespace', compoundName(compound).split('::anonymous_namespace{')[0])
      file.appendChild(copy)
    }
  }
  compounds = compounds.filter(n => !fileScopes.has(attr(n, 'id')))
  // Doxygen materializes copies for using declarations. Keep the declaration's
  // qualified owner and redirect imported type and member links to it.
  const aliases = new Map(fileScopes)
  const declarations = new Map()
  for (const compound of compounds) {
    if (!['class', 'struct', 'union'].includes(attr(compound, 'kind'))) continue
    const location = child(compound, 'location')
    if (!attr(location, 'file') || !attr(location, 'line')) continue
    const key = `${attr(compound, 'kind')}:${attr(location, 'file')}:${attr(location, 'line')}:${attr(location, 'column')}`
    const previous = declarations.get(key)
    const declarationScore = node => {
      const name = compoundName(node)
      const members = descendants(node, 'memberdef').filter(m => value(m, 'definition').includes(`${name}::`)).length
      const directory = dirname(attr(child(node, 'location'), 'file')).replace(/^(include|src)\/?/, '')
      const owner = directory ? `sun::${directory.replaceAll('/', '::')}` : 'sun'
      return [members > 0 ? 1 : 0, name.startsWith(`${owner}::`) ? 1 : 0, name.split('::').length]
    }
    const preferred = (left, right) => {
      const a = declarationScore(left), b = declarationScore(right)
      for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return a[i] > b[i]
      return false
    }
    if (!previous || preferred(compound, previous)) {
      if (previous) aliases.set(attr(previous, 'id'), attr(compound, 'id'))
      declarations.set(key, compound)
    } else aliases.set(attr(compound, 'id'), attr(previous, 'id'))
  }
  const allMembers = new Map(compounds.flatMap(n => descendants(n, 'memberdef')).map(n => [attr(n, 'id'), n]))
  const imported = compounds.filter(n => aliases.has(attr(n, 'id')))
  compounds = compounds.filter(n => !aliases.has(attr(n, 'id')))
  const refs = new Map()
  const owners = new Map()
  const url = compound => `${ROUTE}/${safeId(attr(compound, 'id'))}`
  for (const compound of compounds) refs.set(attr(compound, 'id'), url(compound))
  // Prefer a namespace or type as the canonical owner over a file listing.
  const ownershipOrder = [...compounds].sort((a, b) => Number(attr(a, 'kind') === 'file') - Number(attr(b, 'kind') === 'file'))
  for (const compound of ownershipOrder) {
    for (const member of descendants(compound, 'memberdef')) {
      const id = safeId(attr(member, 'id'))
      if (!owners.has(id)) {
        owners.set(id, compound)
        refs.set(id, `${url(compound)}#${id}`)
        for (const entry of children(member, 'enumvalue')) refs.set(attr(entry, 'id'), `${url(compound)}#${safeId(attr(entry, 'id'))}`)
      }
    }
  }
  const memberDeclarations = new Map()
  const memberKey = member => {
    const location = child(member, 'location')
    const file = attr(location, 'bodyfile') || attr(location, 'file')
    const line = attr(location, 'bodystart') || attr(location, 'line')
    const identity = value(member, 'definition') || (file && line ? value(member, 'name') : attr(member, 'id'))
    return `${attr(member, 'kind')}:${identity}:${value(member, 'argsstring')}:${file}:${line}`
  }
  const declarationMembers = ownershipOrder.flatMap(compound => descendants(compound, 'memberdef').map(member => ({ compound, member })))
  for (const { compound, member } of declarationMembers) {
    const key = memberKey(member)
    const id = attr(member, 'id')
    const location = child(member, 'location')
    const atDefinition = attr(location, 'file') && attr(location, 'file') === attr(location, 'bodyfile') && attr(location, 'line') === attr(location, 'bodystart')
    const score = (value(member, 'definition').includes(`${compoundName(compound)}::${value(member, 'name')}`) ? 2 : 0) + (atDefinition ? 1 : 0)
    const previous = memberDeclarations.get(key)
    if (!previous || score > previous.score) memberDeclarations.set(key, { id, score })
  }
  for (const { member } of declarationMembers) {
    const target = memberDeclarations.get(memberKey(member))?.id
    if (target && target !== attr(member, 'id')) aliases.set(attr(member, 'id'), target)
  }
  for (const compound of imported) {
    for (const member of descendants(compound, 'memberdef')) {
      const target = memberDeclarations.get(memberKey(member))?.id
      if (target) aliases.set(attr(member, 'id'), target)
    }
  }
  const canonicalId = id => {
    const seen = new Set()
    while (aliases.has(id)) {
      if (seen.has(id)) throw new Error(`Cyclic imported symbol: ${id}`)
      seen.add(id)
      id = aliases.get(id)
    }
    return id
  }
  for (const [id] of aliases) {
    const target = canonicalId(id)
    if (refs.has(target)) refs.set(id, refs.get(target))
    if (owners.has(target)) owners.set(id, owners.get(target))
    const member = allMembers.get(id)
    const declaration = allMembers.get(target)
    if (member && declaration) {
      const values = new Map(children(declaration, 'enumvalue').map(n => [value(n, 'name'), attr(n, 'id')]))
      for (const entry of children(member, 'enumvalue')) {
        const destination = refs.get(values.get(value(entry, 'name')))
        if (destination) refs.set(attr(entry, 'id'), destination)
      }
    }
  }
  const pages = new Map()
  const meta = {}
  const groups = [
    ['namespaces', 'Namespaces', 'compound', 'namespace'],
    ['classes', 'Classes', 'compound', 'class'],
    ['structs', 'Structs', 'compound', 'struct'],
    ['enums', 'Enums', 'member', 'enum'],
    ['functions', 'Functions', 'member', 'function'],
    ['type-aliases', 'Type Aliases', 'member', 'typedef'],
    ['unions', 'Unions', 'compound', 'union'],
    ['variables', 'Variables', 'member', 'variable'],
    ['macros', 'Macros', 'member', 'define'],
    ['files', 'Files', 'compound', 'file'],
  ]
  const namespaces = compounds.filter(n => attr(n, 'kind') === 'namespace')
  const namespaceFor = compound => {
    const name = visibleName(compoundName(compound))
    if (attr(compound, 'kind') === 'namespace') return name
    return namespaces.map(compoundName).filter(ns => name.startsWith(`${ns}::`)).sort((a, b) => b.length - a.length)[0] || ''
  }
  for (const [slug, label, source, kind] of groups) {
    const entries = source === 'compound'
      ? compounds.filter(n => attr(n, 'kind') === kind && (kind !== 'namespace' || namedSunNamespace(compoundName(n)))).map(n => [compoundName(n), url(n), namespaceFor(n), attr(child(n, 'location'), 'file')]) : []
    if (source === 'member') {
      for (const compound of compounds) {
        // Methods and fields remain on their owning type's page.
        if (['function', 'variable'].includes(kind) && !['namespace', 'file'].includes(attr(compound, 'kind'))) continue
        for (const member of descendants(compound, 'memberdef')) {
          if (attr(member, 'kind') !== kind || owners.get(attr(member, 'id')) !== compound || aliases.has(attr(member, 'id'))) continue
          const name = value(member, 'qualifiedname') || (attr(compound, 'kind') === 'file' ? value(member, 'name') : `${compoundName(compound)}::${value(member, 'name')}`)
          entries.push([name, refs.get(attr(member, 'id')), attr(member, 'data-sun-namespace') || namespaceFor(compound), attr(child(member, 'location'), 'file') || attr(child(compound, 'location'), 'file') || (attr(compound, 'kind') === 'file' ? compoundName(compound) : '')])
        }
      }
    }
    if (!entries.length) continue
    meta[slug] = { title: label, display: 'hidden' }
    entries.sort((a, b) => compare(a[0], b[0]) || compare(a[1], b[1]))
    pages.set(`${slug}.mdx`, slug === 'namespaces'
      ? renderNamespaceTree(compounds.filter(n => attr(n, 'kind') === 'namespace'), refs)
      : slug === 'files' ? `# ${label}\n\n${entries.map(([name, href]) => `- ${link(name, href)}`).join('\n')}\n` : renderSymbolTree(label, entries, namespaces, refs))
  }
  // Build one hierarchy, attaching nested types and members to their canonical owner.
  const tree = []
  const fileNodes = new Map()
  const fileEntries = compounds.filter(n => attr(n, 'kind') === 'file').map(n => ({ label: compoundName(n), href: url(n), children: [] }))
  const fileLinks = new Map(fileEntries.map(node => [node.label, node.href]))
  const namespaceNodes = new Map()
  const nodes = new Map()
  const group = (parent, label) => {
    let node = parent.children.find(n => n.label === label && !n.href)
    if (!node) parent.children.push(node = { label, children: [] })
    return node
  }
  const scope = name => {
    name = visibleName(name)
    if (!namedSunNamespace(name)) name = 'File scope'
    if (namespaceNodes.has(name)) return namespaceNodes.get(name)
    const split = name.lastIndexOf('::')
    const node = { label: split < 0 ? name : name.slice(split + 2), ...(namedSunNamespace(name) ? { kind: 'namespace' } : {}), children: [] }
    namespaceNodes.set(name, node)
    if (split < 0) tree.push(node)
    else scope(name.slice(0, split)).children.push(node)
    return node
  }
  const fileScope = file => {
    if (!file) return scope('')
    if (!fileNodes.has(file)) fileNodes.set(file, { label: file, ...(fileLinks.has(file) ? { href: fileLinks.get(file) } : {}), children: [] })
    return fileNodes.get(file)
  }
  for (const namespace of namespaces.filter(n => namedSunNamespace(compoundName(n)))) {
    scope(compoundName(namespace)).href = url(namespace)
  }
  const types = compounds.filter(n => ['class', 'struct', 'union'].includes(attr(n, 'kind')))
  for (const type of types) nodes.set(attr(type, 'id'), { label: visibleName(compoundName(type)), href: url(type), children: [] })
  for (const type of types) {
    const name = compoundName(type)
    const parentType = types.filter(n => n !== type && name.startsWith(`${compoundName(n)}::`)).sort((a, b) => compoundName(b).length - compoundName(a).length)[0]
    const namespace = namespaceFor(type)
    const parent = parentType ? nodes.get(attr(parentType, 'id')) : namespace ? scope(namespace) : fileScope(attr(child(type, 'location'), 'file'))
    const label = { class: 'Classes', struct: 'Structs', union: 'Unions' }[attr(type, 'kind')]
    const node = nodes.get(attr(type, 'id'))
    const ownerName = parentType ? visibleName(compoundName(parentType)) : namespace
    if (ownerName && node.label.startsWith(`${ownerName}::`)) node.label = node.label.slice(ownerName.length + 2)
    group(parent, label).children.push(node)
  }
  for (const compound of compounds) {
    for (const member of new Map(descendants(compound, 'memberdef').map(n => [attr(n, 'id'), n])).values()) {
      const id = attr(member, 'id')
      if (owners.get(id) !== compound || aliases.has(id)) continue
      const type = nodes.get(attr(compound, 'id'))
      const namespace = attr(member, 'data-sun-namespace') || namespaceFor(compound)
      const parent = type || (namespace && namedSunNamespace(visibleName(namespace)) ? scope(namespace) : fileScope(attr(child(member, 'location'), 'file') || attr(child(compound, 'location'), 'file') || (attr(compound, 'kind') === 'file' ? compoundName(compound) : '')))
      const label = type ? memberGroup(member, attr(compound, 'kind')) : ({ variable: 'Variables', define: 'Macros' }[attr(member, 'kind')] || memberKinds[attr(member, 'kind')] || 'Other Symbols')
      const node = { label: value(member, 'name'), href: refs.get(id), children: [] }
      const values = children(member, 'enumvalue')
      if (values.length) group(node, 'Enum Values').children.push(...values.map(entry => ({ label: value(entry, 'name'), href: refs.get(attr(entry, 'id')), children: [] })))
      group(parent, label).children.push(node)
    }
  }
  if (fileNodes.size) scope('').children.push(...fileNodes.values())
  if (fileEntries.length) tree.push({ label: 'Files', children: fileEntries })
  const categoryOrder = ['Classes', 'Structs', 'Unions', 'Functions', 'Variables', 'Enums', 'Type Aliases', 'Macros', ...['Public', 'Protected', 'Private'].flatMap(access => Object.values(memberKinds).map(kind => `${access} ${kind}`))]
  const sortTree = nodes => {
    const rank = node => node.href || !categoryOrder.includes(node.label) ? -1 : categoryOrder.indexOf(node.label)
    nodes.sort((a, b) => rank(a) - rank(b) || compare(a.label, b.label) || compare(a.href || '', b.href || ''))
    for (const node of nodes) sortTree(node.children)
  }
  for (const node of tree) sortTree(node.children)
  tree.sort((a, b) => Number(b.label === 'sun') - Number(a.label === 'sun') || compare(a.label, b.label))
  pages.set('tree.json', JSON.stringify(tree))
  pages.set('index.mdx', `import { CompilerApiTree } from '../components/compiler-api-tree'\nimport tree from './compiler-api/tree.json'\n\n# Compiler API Reference\n\n<CompilerApiTree nodes={tree} />\n`)
  const compoundKinds = new Map(compounds.map(n => [attr(n, 'id'), attr(n, 'kind')]))
  for (const compound of compounds) {
    const id = attr(compound, 'id')
    const name = visibleName(compoundName(compound))
    const isNamespace = attr(compound, 'kind') === 'namespace'
    // Indexes provide navigation without adding hundreds of sidebar entries.
    meta[id] = { title: name, display: 'hidden' }
    const declaration = ['class', 'struct', 'union'].includes(attr(compound, 'kind'))
      ? fence(`${template(compound)}${attr(compound, 'kind')} ${name}${children(compound, 'basecompoundref').length ? ' : ' + children(compound, 'basecompoundref').map(n => `${attr(n, 'prot')} ${visibleName(n.textContent)}`).join(', ') : ''}`) : ''
    let page = `# ${escape(name)}\n\n${escape(attr(compound, 'kind'))}${compoundName(compound).includes('anonymous_namespace{') ? ' · file-local' : ''} · ${sourceLink(compound, revision)}\n\n${declaration}${description(compound, refs)}\n\n`
    for (const [tag, label] of [['basecompoundref', 'Inherits'], ['derivedcompoundref', 'Inherited by'], ['innernamespace', 'Namespaces'], ['innerclass', 'Types']]) {
      const entries = children(compound, tag).filter(n => (tag !== 'innernamespace' || namedSunNamespace(n.textContent)) && (tag !== 'innerclass' || !aliases.has(attr(n, 'refid'))))
      const entryGroups = isNamespace && tag === 'innerclass'
        ? ['class', 'struct', 'union'].map(kind => [{ class: 'Classes', struct: 'Structs', union: 'Unions' }[kind], entries.filter(n => compoundKinds.get(attr(n, 'refid')) === kind)])
        : [[label, entries]]
      for (const [heading, items] of entryGroups) {
        if (items.length) page += `## ${heading}\n\n${items.map(n => `- ${refs.has(attr(n, 'refid')) ? link(visibleName(n.textContent), refs.get(attr(n, 'refid'))) : escape(visibleName(n.textContent))}${attr(n, 'prot') ? ` (${escape(attr(n, 'prot'))})` : ''}`).join('\n')}\n\n`
      }
    }
    const members = [...new Map(descendants(compound, 'memberdef').map(n => [attr(n, 'id'), n])).values()]
      .sort((a, b) => compare(attr(a, 'kind'), attr(b, 'kind')) || compare(value(a, 'name'), value(b, 'name')) || compare(attr(a, 'id'), attr(b, 'id')))
    const memberGroups = new Map()
    for (const member of members) {
      const label = declaration ? memberGroup(member, attr(compound, 'kind')) : isNamespace ? ({ variable: 'Variables', define: 'Macros' }[attr(member, 'kind')] || memberKinds[attr(member, 'kind')] || 'Other Symbols') : 'Members'
      if (!memberGroups.has(label)) memberGroups.set(label, [])
      memberGroups.get(label).push(member)
    }
    const sectionOrder = isNamespace ? ['Functions', 'Variables', 'Enums', 'Type Aliases', 'Macros', 'Friends', 'Other Symbols'] : ['Public', 'Protected', 'Private'].flatMap(access => [...Object.values(memberKinds), 'Other Members'].map(kind => `${access} ${kind}`))
    for (const [label, entries] of [...memberGroups].sort(([a], [b]) => sectionOrder.indexOf(a) - sectionOrder.indexOf(b))) {
      page += `## ${label}\n\n${entries.map(n => `- ${link(value(n, 'name'), refs.get(attr(n, 'id')))}`).join('\n')}\n\n`
      for (const member of entries) {
        const memberId = attr(member, 'id')
        if (owners.get(memberId) !== compound || aliases.has(memberId)) continue
        page += `<a id="${memberId}" />\n\n### ${escape(value(member, 'name'))}\n\n${[attr(member, 'prot'), attr(member, 'kind'), attr(member, 'static') === 'yes' ? 'static' : ''].filter(Boolean).join(' · ')} · ${sourceLink(member, revision)}\n${fence(visibleName(signature(member)))}${description(member, refs)}\n\n`
        const typeRefs = [...new Map(descendants(member, 'ref').filter(n => refs.has(attr(n, 'refid'))).map(n => [attr(n, 'refid'), n])).values()]
        if (typeRefs.length) page += `Related: ${typeRefs.map(n => link(visibleName(n.textContent), refs.get(attr(n, 'refid')))).join(', ')}\n\n`
        for (const entry of children(member, 'enumvalue')) page += `<a id="${safeId(attr(entry, 'id'))}" />\n\n#### ${escape(value(entry, 'name'))}\n\n${code(`${value(entry, 'name')} ${value(entry, 'initializer')}`.trim())}\n\n${description(entry, refs)}\n\n`
      }
    }
    pages.set(`${id}.mdx`, page)
  }
  pages.set('_meta.json', JSON.stringify(meta, null, 2) + '\n')
  return pages
}

/** Replaces generated pages, removing entries for symbols that no longer exist. */
export function writeReference(output, pages) {
  mkdirSync(output, { recursive: true })
  for (const file of readdirSync(output)) if (!pages.has(file)) rmSync(join(output, file), { recursive: true, force: true })
  for (const [file, content] of pages) writeFileSync(join(output, file), content)
}

/** Extracts compiler documentation and updates the generated Nextra section. */
export function generate() {
  const executable = process.env.DOXYGEN || 'doxygen'
  let version
  try { version = execFileSync(executable, ['--version'], { encoding: 'utf8' }).trim() }
  catch { throw new Error(`Doxygen ${VERSION} is required. See docs/compiler-api/README.md; set DOXYGEN to its executable path.`) }
  if (version.split(/\s/)[0] !== VERSION) throw new Error(`Expected Doxygen ${VERSION}, found ${version}`)
  const revision = execFileSync('git', ['rev-parse', 'HEAD'], { cwd: ROOT, encoding: 'utf8' }).trim()
  const work = join(ROOT, 'docs/generated/compiler-api')
  rmSync(work, { recursive: true, force: true })
  mkdirSync(work, { recursive: true })
  const config = readFileSync(join(ROOT, 'docs/compiler-api/Doxyfile'), 'utf8')
  execFileSync(executable, ['-'], { cwd: ROOT, input: `${config}\nOUTPUT_DIRECTORY = "${work}"\nWARN_LOGFILE = "${work}/warnings.log"\n`, stdio: ['pipe', 'inherit', 'inherit'] })
  const xml = join(work, 'xml')
  const index = parseXml(readFileSync(join(xml, 'index.xml'), 'utf8'))
  const compounds = descendants(index, 'compound').filter(n => kinds.has(attr(n, 'kind'))).map(n => {
    const doc = parseXml(readFileSync(join(xml, `${safeId(attr(n, 'refid'))}.xml`), 'utf8'))
    const compound = descendants(doc, 'compounddef')[0]
    if (!compound) throw new Error(`Missing compound definition: ${attr(n, 'refid')}`)
    return compound
  }).filter(compound => {
    if (attr(compound, 'kind') !== 'namespace') return true
    if (/^sun::proto(?:::|$)/.test(compoundName(compound))) return false
    if (!/^sun(?:::|$)/.test(compoundName(compound)) && !/anonymous_namespace/.test(compoundName(compound))) return false
    const location = child(compound, 'location')
    const file = attr(location, 'file')
    if (!/^(include|src)\//.test(file) || file.split('/').includes('..')) return false
    const line = readFileSync(join(ROOT, file), 'utf8').split('\n')[Number(attr(location, 'line')) - 1] || ''
    // Doxygen also creates namespace entries for using directives and aliases.
    return /^\s*(?:inline\s+)?namespace\s*[^=]*\{?\s*$/.test(line) && !/\busing\b/.test(line)
  })
  if (!compounds.length) throw new Error('Doxygen produced no compiler symbols')
  const pages = renderReference(compounds, revision)
  const landing = pages.get('index.mdx')
  pages.delete('index.mdx')
  writeReference(join(ROOT, 'docs/pages/compiler-api'), pages)
  writeFileSync(join(ROOT, 'docs/pages/compiler-api.mdx'), landing)
  const warnings = readFileSync(join(work, 'warnings.log'), 'utf8').trim()
  if (warnings) console.warn(`Doxygen reported warnings; see ${relative(ROOT, work)}/warnings.log`)
  console.log(`Generated compiler reference: ${compounds.length} compounds, ${pages.size} pages.`)
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) generate()
