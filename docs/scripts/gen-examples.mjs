// Generates docs pages from the programs in the repo-root examples/ folder.
// Every examples/<name>/ has a README.md plus build.sh and test.sh that CI
// compiles and runs. Two outputs:
//
//  - docs/pages/examples.mdx: the Examples page, one section per example.
//    Ordering comes from the numeric folder-name prefix (10-classes, ...).
//  - docs/generated/<name>.mdx: an MDX partial for each example whose README
//    starts with a `docs-page: <page>` frontmatter block. These examples are
//    left off the Examples page; instead the named handwritten page imports
//    the partial (modules.mdx imports generated/path_variables.mdx). Such
//    folders carry no numeric prefix — they hold no slot on the Examples
//    page — and the host page owns the heading, so the README's title is
//    dropped and everything under it sits two levels deeper.
//
// Each example is rendered from its README.md (the prose the docs show) plus
// every .sun source file found in the folder, read verbatim so the docs can
// never drift from the code CI compiles and runs. The README is the single
// source of truth for prose, and the source list is discovered by globbing
// *.sun.
//
// A README is written as an intro, then `## ` sections — by convention a
// single `## Build and run` holding the commands. The source is spliced in
// between the two, so a reader on the docs site meets the program before the
// commands that build it. Those sections are pushed down to sit alongside the
// generated `Source` heading, which is one level deeper on the Examples page
// than in a partial.
//
// Run automatically via the `predev` / `prebuild` npm scripts. CWD is docs/.

import { readFileSync, writeFileSync, readdirSync, existsSync, mkdirSync } from 'node:fs'
import { join, extname, dirname, basename, relative, sep } from 'node:path'
import { fileURLToPath } from 'node:url'

const DOCS_DIR = join(dirname(fileURLToPath(import.meta.url)), '..')
const EXAMPLES_DIR = join(DOCS_DIR, '..', 'examples')
const GENERATED_DIR = join(DOCS_DIR, 'generated')
const OUT_FILE = join(DOCS_DIR, 'pages', 'examples.mdx')

// Map a source file extension to a fenced-code language for syntax highlighting.
function langFor(file) {
  const ext = extname(file).slice(1)
  if (ext === 'sun' || ext === 'moon') return 'sun'
  return ext || 'text'
}

// Recursively collect every .sun source under `dir`, returned as paths relative
// to `dir` and sorted so top-level main.sun leads and nested moons follow.
function collectSunFiles(dir, base = dir) {
  const out = []
  for (const entry of readdirSync(dir, { withFileTypes: true })) {
    const full = join(dir, entry.name)
    if (entry.isDirectory()) {
      out.push(...collectSunFiles(full, base))
    } else if (extname(entry.name) === '.sun') {
      out.push(relative(base, full).split(sep).join('/'))
    }
  }
  return out.sort()
}

// Split an optional leading `--- key: value ---` frontmatter block from the
// README, returning its keys and the remaining markdown.
function splitFrontmatter(readme) {
  const match = readme.match(/^---\n([\s\S]*?)\n---\n/)
  if (!match) return { meta: {}, markdown: readme }
  const meta = {}
  for (const line of match[1].split('\n')) {
    const kv = line.match(/^([\w-]+):\s*(.*)$/)
    if (kv) meta[kv[1]] = kv[2].trim()
  }
  return { meta, markdown: readme.slice(match[0].length) }
}

// An example is any immediate subdirectory that has a README.md. A plain name
// sort gives the ordering.
function loadExamples() {
  return readdirSync(EXAMPLES_DIR, { withFileTypes: true })
    .filter((d) => d.isDirectory())
    .map((d) => join(EXAMPLES_DIR, d.name))
    .filter((dir) => existsSync(join(dir, 'README.md')))
    .sort()
    .map((dir) => {
      const { meta, markdown } = splitFrontmatter(readFileSync(join(dir, 'README.md'), 'utf8'))
      return { dir, meta, readme: markdown }
    })
}

// Walk the lines of a markdown document, reporting for each whether it sits
// inside a fenced code block. A `# ...` line inside a fence is shell output or
// a comment, never a heading.
function* markdownLines(markdown) {
  let fence = null
  for (const line of markdown.split('\n')) {
    const marker = line.match(/^\s*(```+|~~~+)/)
    if (fence) {
      if (marker && marker[1].startsWith(fence)) fence = null
      yield { line, inFence: true }
    } else if (marker) {
      fence = marker[1]
      yield { line, inFence: true }
    } else {
      yield { line, inFence: false }
    }
  }
}

// Push every heading `by` levels deeper, so a README's `## Build and run`
// nests under the heading the example is rendered with.
function shiftHeadings(markdown, by) {
  const out = []
  for (const { line, inFence } of markdownLines(markdown)) {
    const heading = inFence ? null : line.match(/^(#{1,6})(\s+.*)$/)
    out.push(heading ? '#'.repeat(Math.min(heading[1].length + by, 6)) + heading[2] : line)
  }
  return out.join('\n')
}

// Split the README into its H1 title, the intro prose under it, and the `## `
// sections that follow — the source is spliced between the last two.
function splitReadme(readme) {
  const lines = [...markdownLines(readme)]
  const titleAt = lines.findIndex(({ line, inFence }) => !inFence && /^#\s+/.test(line))
  const title = titleAt === -1 ? null : lines[titleAt].line.replace(/^#\s+/, '').trim()

  const rest = lines.slice(titleAt + 1)
  const sectionsAt = rest.findIndex(({ line, inFence }) => !inFence && /^##\s+/.test(line))
  const take = (from, to) =>
    rest.slice(from, to).map(({ line }) => line).join('\n').trim()

  if (sectionsAt === -1) return { title, intro: take(0), sections: '' }
  return { title, intro: take(0, sectionsAt), sections: take(sectionsAt) }
}

// A `Source` heading at the given level, followed by every .sun file verbatim.
function renderSources(dir, level) {
  const parts = ['#'.repeat(level) + ' Source', '']
  for (const file of collectSunFiles(dir)) {
    const contents = readFileSync(join(dir, file), 'utf8').replace(/\s+$/, '')
    parts.push('```' + langFor(file) + ` filename="${file}"`)
    parts.push(contents)
    parts.push('```', '')
  }
  return parts
}

// The body of one example: its intro prose, then the source, then the README's
// own sections at `level` — the same level the `Source` heading is given.
function renderBody({ dir, readme }, level) {
  const { intro, sections } = splitReadme(readme)
  const parts = []
  if (intro) parts.push(intro, '')
  parts.push(...renderSources(dir, level))
  if (sections) parts.push(shiftHeadings(sections, level - 2), '')
  return parts
}

// A section of the Examples page: the README title becomes a level-2 heading
// so it nests under the page's `# Examples`.
function renderExample(example) {
  return [`## ${splitReadme(example.readme).title}`, '', ...renderBody(example, 3)].join('\n')
}

// A partial for a handwritten page to import. The host page owns the heading,
// so the README's H1 is dropped and `Source` sits at level 4.
function renderPartial(example) {
  const name = basename(example.dir)
  const parts = [
    `{/* AUTO-GENERATED by docs/scripts/gen-examples.mjs from examples/${name}/. */}`,
    '{/* Do not edit by hand — edit the program in that folder instead. */}',
    '',
  ]
  parts.push(...renderBody(example, 4))
  return parts.join('\n')
}

const examples = loadExamples()
const pageExamples = examples.filter((e) => !e.meta['docs-page'])
const partials = examples.filter((e) => e.meta['docs-page'])

const header = [
  '{/* AUTO-GENERATED by docs/scripts/gen-examples.mjs from the examples/ folder. */}',
  '{/* Do not edit by hand — edit the programs under examples/ instead. */}',
  '',
  '# Examples',
  '',
  'Complete, runnable programs demonstrating Sun. Every example below is compiled',
  'and executed in CI, and the source shown here is the exact source in the',
  '[`examples/`](https://github.com/namo-robotics/sun/tree/main/examples) folder.',
  '',
].join('\n')

writeFileSync(OUT_FILE, header + '\n' + pageExamples.map(renderExample).join('\n'))
console.log(`Generated ${OUT_FILE} from ${pageExamples.length} examples.`)

mkdirSync(GENERATED_DIR, { recursive: true })
for (const example of partials) {
  const name = basename(example.dir)
  const page = example.meta['docs-page']
  // A partial is only visible if its host page imports it, so a docs-page that
  // names a missing or non-importing page would silently drop the example.
  const host = join(DOCS_DIR, 'pages', page + '.mdx')
  if (!existsSync(host)) {
    throw new Error(`examples/${name}/README.md names docs-page "${page}", but pages/${page}.mdx does not exist.`)
  }
  if (!readFileSync(host, 'utf8').includes(`generated/${name}.mdx`)) {
    throw new Error(
      `examples/${name}/README.md names docs-page "${page}", but pages/${page}.mdx never imports generated/${name}.mdx, ` +
        `so the example would appear nowhere. Add the import, or drop the frontmatter to list it on the Examples page.`,
    )
  }
  const out = join(GENERATED_DIR, name + '.mdx')
  writeFileSync(out, renderPartial(example))
  console.log(`Generated ${out} for the ${page} page.`)
}
