import assert from 'node:assert/strict'
import test from 'node:test'
import { execFileSync } from 'node:child_process'
import { mkdtempSync, mkdirSync, readFileSync, readdirSync, writeFileSync, rmSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { join } from 'node:path'
import { compile } from '@mdx-js/mdx'
import { parseXml, renderReference, renderNamespaceTree, renderSymbolTree, writeReference } from './gen-compiler-api.mjs'

const root = fileURLToPath(new URL('../../', import.meta.url))
const revision = 'a'.repeat(40)
const compound = xml => parseXml(xml).documentElement
const fixture = () => [
  compound(`<compounddef id="namespace_demo" kind="namespace"><compoundname>sun::demo</compoundname>
    <innerclass refid="class_demo">sun::demo::Box</innerclass>
    <sectiondef><memberdef id="helper" kind="function" static="yes" prot="public">
      <type>int</type><definition>int sun::demo::helper</definition><name>helper</name><argsstring>()</argsstring>
      <briefdescription><para>Find a <ref refid="class_demo">Box</ref>.</para></briefdescription>
      <location file="src/demo.cpp" line="12"/>
    </memberdef></sectiondef></compounddef>`),
  compound(`<compounddef id="class_demo" kind="class"><compoundname>sun::demo::Box</compoundname>
    <basecompoundref refid="struct_base" prot="public">Base</basecompoundref>
    <briefdescription><para>Stores &lt;values&gt; with {braces}, [brackets], and backticks &#96;.</para></briefdescription>
    <detaileddescription><para>Another paragraph.</para><para>import something from 'invalid'</para>
    <programlisting><codeline><highlight>Box<sp/>box;</highlight></codeline></programlisting><verbatim>&#96;&#96;&#96;
&lt;script&gt;{example}&lt;/script&gt;
&#96;&#96;&#96;</verbatim></detaileddescription>
    <sectiondef>
      <memberdef id="read_int" kind="function" prot="private"><type>int</type><definition>int sun::demo::Box::read</definition><name>read</name><argsstring>(int value)</argsstring>
        <detaileddescription><para>Reads a value.</para><parameterlist kind="param"><parameteritem><parameternamelist><parametername>value</parametername></parameternamelist><parameterdescription><para>The input.</para></parameterdescription></parameteritem></parameterlist><simplesect kind="return"><para>The result.</para></simplesect></detaileddescription>
        <location file="include/demo.h" line="8"/>
      </memberdef>
      <memberdef id="read_bool" kind="function" prot="public"><type>bool</type><definition>bool sun::demo::Box::read</definition><name>read</name><argsstring>(bool value)</argsstring></memberdef>
      <memberdef id="alias" kind="typedef" prot="public"><type><ref refid="struct_base">Base</ref></type><definition>using sun::demo::Box::Value = Base</definition><name>Value</name></memberdef>
      <memberdef id="mode" kind="enum" prot="public"><definition>enum sun::demo::Box::Mode</definition><name>Mode</name><enumvalue id="ready"><name>Ready</name><initializer>= 1</initializer><briefdescription><para>Can read.</para></briefdescription></enumvalue></memberdef>
    </sectiondef></compounddef>`),
  compound('<compounddef id="struct_base" kind="struct"><compoundname>Base</compoundname></compounddef>'),
  compound('<compounddef id="file_demo" kind="file"><compoundname>src/demo.cpp</compoundname><sectiondef><memberdef id="helper" kind="function"><name>helper</name><argsstring>()</argsstring></memberdef></sectiondef></compounddef>'),
]

function checkLinks(pages) {
  for (const page of pages.values()) {
    for (const [, slug, anchor] of page.matchAll(/(?:\]\(|href=")\/compiler-api\/([^\s)#"]+)(?:#([^\s)"]+))?[)"]/g)) {
      const target = pages.get(`${slug}.mdx`)
      assert.ok(target, `Missing page ${slug}`)
      if (anchor) assert.ok(target.includes(`id="${anchor}"`), `Missing anchor ${anchor}`)
    }
  }
}

test('renders source descriptions, internals, overloads, inheritance, enums, aliases and canonical links', async () => {
  const pages = renderReference(fixture(), revision)
  const box = pages.get('class_demo.mdx')
  assert.match(box, /private · function/)
  assert.match(box, /id="read_int"/)
  assert.match(box, /id="read_bool"/)
  assert.match(box, /Inherits[\s\S]*Base/)
  assert.match(box, /using sun::demo::Box::Value = Base/)
  assert.match(box, /id="ready"/)
  assert.match(pages.get('enums.mdx'), /class_demo#mode/)
  assert.match(pages.get('type-aliases.mdx'), /class_demo#alias/)
  assert.match(box, /No documentation comment/)
  assert.match(box, /The input/)
  assert.match(box, /The result/)
  assert.match(box, /Another paragraph/)
  assert.match(box, /Box box;/)
  assert.match(box, /&#60;values&#62; with &#123;braces&#125;/)
  assert.match(box, new RegExp(`blob/${revision}/include/demo.h#L8`))
  assert.doesNotMatch(pages.get('file_demo.mdx'), /id="helper"/)
  assert.match(pages.get('file_demo.mdx'), /namespace_demo#helper/)
  assert.match(pages.get('classes.mdx'), /class_demo/)
  assert.match(pages.get('structs.mdx'), /struct_base/)
  assert.match(pages.get('functions.mdx'), /namespace_demo#helper/)
  assert.doesNotMatch(pages.get('functions.mdx'), /read_int|read_bool/)
  assert.ok(!pages.has('types.mdx'))
  assert.ok(!pages.has('unions.mdx'))
  checkLinks(pages)
  for (const [name, page] of pages) if (name.endsWith('.mdx')) await compile(page)
  assert.deepEqual(renderReference(fixture().reverse(), revision), pages)
})

test('builds one categorized tree with nested types, members, enum values and stable links', async () => {
  const input = fixture()
  input.push(compound('<compounddef id="nested" kind="struct"><compoundname>sun::demo::Box::Nested</compoundname></compounddef>'))
  input.push(compound('<compounddef id="hash_box" kind="struct"><compoundname>std::hash&lt; sun::demo::Box &gt;</compoundname><location file="src/demo.cpp" line="20" /></compounddef>'))
  input.push(compound('<compounddef id="templated" kind="struct"><compoundname>sun::demo::Wrapper&lt; sun::demo::Box &gt;</compoundname></compounddef>'))
  const section = input[1].getElementsByTagName('sectiondef')[0]
  section.appendChild(section.getElementsByTagName('memberdef')[0].cloneNode(true))
  const pages = renderReference(input, revision)
  const landing = pages.get('index.mdx')
  const tree = JSON.parse(pages.get('tree.json'))
  assert.ok(landing.length < 300)
  assert.match(landing, /import tree from '.\/compiler-api\/tree.json'/)
  const get = (node, label) => {
    const found = node.children.find(child => child.label === label)
    assert.ok(found, `Missing ${label} under ${node.label}`)
    return found
  }
  const sun = tree.find(node => node.label === 'sun')
  assert.equal(sun.kind, 'namespace')
  const demo = get(sun, 'demo')
  assert.equal(demo.kind, 'namespace')
  assert.equal(get(get(demo, 'Structs'), 'Wrapper< sun::demo::Box >').href, '/compiler-api/templated')
  const files = tree.find(node => node.label === 'File scope')
  assert.equal(get(get(get(files, 'src/demo.cpp'), 'Structs'), 'std::hash< sun::demo::Box >').href, '/compiler-api/hash_box')
  const box = get(get(demo, 'Classes'), 'Box')
  assert.equal(box.kind, undefined)
  assert.equal(get(get(box, 'Public Functions'), 'read').href, '/compiler-api/class_demo#read_bool')
  assert.equal(get(get(box, 'Private Functions'), 'read').href, '/compiler-api/class_demo#read_int')
  assert.equal(get(get(box, 'Structs'), 'Nested').href, '/compiler-api/nested')
  assert.equal(get(get(get(get(box, 'Public Enums'), 'Mode'), 'Enum Values'), 'Ready').href, '/compiler-api/class_demo#ready')
  assert.equal(get(get(demo, 'Functions'), 'helper').href, '/compiler-api/namespace_demo#helper')
  const links = new Set()
  const visit = node => {
    if (node.href) {
      assert.ok(!links.has(node.href) || !node.href.includes('#'), `Duplicate ${node.href}`)
      links.add(node.href)
      const [slug, anchor] = node.href.replace('/compiler-api/', '').split('#')
      const target = pages.get(`${slug}.mdx`)
      assert.ok(target, node.href)
      if (anchor) assert.ok(target.includes(`id="${anchor}"`), node.href)
    }
    node.children.forEach(visit)
  }
  tree.forEach(visit)
  assert.ok(Object.values(JSON.parse(pages.get('_meta.json'))).every(entry => entry.display === 'hidden'))
  assert.deepEqual(renderReference(input.reverse(), revision), pages)
  await compile(landing)
})


test('groups global symbols by their source file without merging matching names', () => {
  const input = ['src/first.cpp', 'src/second.cpp'].map((file, index) => compound(`<compounddef id="file_${index}" kind="file"><compoundname>${file}</compoundname><location file="${file}"/><sectiondef><memberdef id="helper_${index}" kind="function"><name>helper</name><argsstring>()</argsstring><location file="${file}"/></memberdef></sectiondef></compounddef>`))
  input.push(compound('<compounddef id="empty_file" kind="file"><compoundname>src/empty.cpp</compoundname></compounddef>'))
  const pages = renderReference(input, revision)
  const tree = JSON.parse(pages.get('tree.json'))
  assert.deepEqual(tree.find(node => node.label === 'Files').children.map(node => node.label), ['src/empty.cpp', 'src/first.cpp', 'src/second.cpp'])
  const scope = tree.find(node => node.label === 'File scope')
  assert.deepEqual(scope.children.map(node => node.label), ['src/first.cpp', 'src/second.cpp'])
  for (const [index, file] of scope.children.entries()) {
    assert.equal(file.href, `/compiler-api/file_${index}`)
    assert.deepEqual(file.children[0].children, [{ label: 'helper', href: `/compiler-api/file_${index}#helper_${index}`, children: [] }])
    assert.ok(pages.get('functions.mdx').includes(`<TreeSummary>${file.label}</TreeSummary>`))
  }
  checkLinks(pages)
})

test('splits namespace types and members into symbol categories', async () => {
  const types = ['class', 'struct', 'union'].map(kind => compound(`<compounddef id="${kind}_item" kind="${kind}"><compoundname>sun::demo::${kind}Item</compoundname></compounddef>`))
  const members = ['function', 'variable', 'enum', 'typedef'].map(kind => `<memberdef id="${kind}_member" kind="${kind}"><definition>${kind}Item</definition><name>${kind}Item</name></memberdef>`).join('')
  const namespace = compound(`<compounddef id="namespace_demo" kind="namespace"><compoundname>sun::demo</compoundname>${['class', 'struct', 'union'].map(kind => `<innerclass refid="${kind}_item">sun::demo::${kind}Item</innerclass>`).join('')}<sectiondef>${members}</sectiondef></compounddef>`)
  const pages = renderReference([namespace, ...types], revision)
  const page = pages.get('namespace_demo.mdx')
  assert.doesNotMatch(page, /^## (Types|Members)$/m)
  const sections = new Map(page.split(/^## /m).slice(1).map(section => [section.split('\n')[0], section]))
  assert.deepEqual([...sections.keys()], ['Classes', 'Structs', 'Unions', 'Functions', 'Variables', 'Enums', 'Type Aliases'])
  for (const [kind, title] of [['class', 'Classes'], ['struct', 'Structs'], ['union', 'Unions']]) assert.ok(sections.get(title).includes(`/compiler-api/${kind}_item`))
  for (const [kind, title] of [['function', 'Functions'], ['variable', 'Variables'], ['enum', 'Enums'], ['typedef', 'Type Aliases']]) {
    assert.ok(sections.get(title).includes(`id="${kind}_member"`))
    assert.equal((sections.get(title).match(/<a id=/g) || []).length, 1)
  }
  checkLinks(pages)
  await compile(page)
})

test('groups type members and their documentation by access and kind', async () => {
  for (const kind of ['class', 'struct', 'union']) {
    const members = ['public', 'protected', 'private'].flatMap(access => ['function', 'variable', 'enum', 'typedef'].map(memberKind => {
      const id = `${access}_${memberKind}`
      return `<memberdef id="${id}" kind="${memberKind}" prot="${access}"><definition>${id}</definition><name>${id}</name><briefdescription><para>Documentation for ${id}.</para></briefdescription></memberdef>`
    })).join('')
    const pages = renderReference([compound(`<compounddef id="example" kind="${kind}"><compoundname>sun::Example</compoundname><sectiondef>${members}</sectiondef></compounddef>`)], revision)
    const page = pages.get('example.mdx')
    assert.doesNotMatch(page, /^## Members$/m)
    const sections = new Map(page.split(/^## /m).slice(1).map(section => [section.split('\n')[0], section]))
    assert.equal(sections.size, 12)
    for (const access of ['public', 'protected', 'private']) {
      for (const [memberKind, label] of [['function', 'Functions'], ['variable', 'Fields'], ['enum', 'Enums'], ['typedef', 'Type Aliases']]) {
        const section = sections.get(`${access[0].toUpperCase() + access.slice(1)} ${label}`)
        assert.ok(section)
        const id = `${access}_${memberKind}`
        assert.ok(section.includes(`id="${id}"`))
        assert.ok(section.includes(`#${id})`))
        assert.equal((section.match(/<a id=/g) || []).length, 1)
        assert.ok(section.includes(`Documentation for ${id.replaceAll('_', '&#95;')}.`))
      }
    }
    checkLinks(pages)
    await compile(page)
  }
})

test('groups every symbol category under named Sun namespaces', async () => {
  const namespaces = [
    compound('<compounddef id="sun" kind="namespace"><compoundname>sun</compoundname></compounddef>'),
    compound('<compounddef id="inner" kind="namespace"><compoundname>sun::ast::nodes</compoundname></compounddef>'),
    compound('<compounddef id="hidden" kind="namespace"><compoundname>sun::ast::anonymous_namespace{file.cpp}</compoundname></compounddef>'),
    compound('<compounddef id="external" kind="namespace"><compoundname>std</compoundname></compounddef>'),
  ]
  const refs = new Map(namespaces.map(n => [n.getAttribute('id'), `/compiler-api/${n.getAttribute('id')}`]))
  const tree = renderNamespaceTree(namespaces, refs)
  assert.equal((tree.match(/<TreeBranch open>/g) || []).length, 1)
  assert.match(tree, /<TreeBranch>\s*<TreeSummary>ast<\/TreeSummary>/)
  assert.match(tree, /href="\/compiler-api\/inner">nodes</)
  assert.doesNotMatch(tree, /anonymous|external|>std</)
  const symbols = renderSymbolTree('Classes', [
    ['sun::ast::nodes::Expr<T>', '/compiler-api/expr', 'sun::ast::nodes'],
    ['helper()', '/compiler-api/helper', 'sun::ast::anonymous_namespace{file.cpp}'],
    ['main', '/compiler-api/main', '', 'src/main.cpp'],
  ], namespaces, refs)
  assert.match(symbols, /Expr&#60;T&#62;/)
  assert.match(symbols, /<TreeSummary>File scope<\/TreeSummary>/)
  assert.match(symbols, /<TreeSummary>src\/main.cpp<\/TreeSummary>/)
  assert.match(symbols, /href="\/compiler-api\/helper"/)
  assert.doesNotMatch(symbols, /anonymous/)
  await compile(tree)
  await compile(symbols)
  const pages = renderReference(fixture(), revision)
  for (const category of ['classes', 'enums', 'functions', 'type-aliases']) {
    assert.match(pages.get(`${category}.mdx`), /<TreeBranch open>/)
    assert.match(pages.get(`${category}.mdx`), /href="\/compiler-api\/namespace_demo">demo</)
  }
})

test('keeps same-spelled declarations at different source locations separate', () => {
  const compounds = ['First', 'Second'].map((name, i) => compound(`<compounddef id="${name}" kind="class"><compoundname>sun::${name}</compoundname><sectiondef><memberdef id="${name}_friend" kind="friend"><definition>friend class Registry</definition><name>Registry</name><location file="include/types.h" line="${i + 1}" /></memberdef></sectiondef></compounddef>`))
  const pages = renderReference(compounds, revision)
  for (const name of ['First', 'Second']) assert.match(pages.get(`${name}.mdx`), new RegExp(`id="${name}_friend"`))
  checkLinks(pages)
})

test('rejects malformed XML and unsafe identifiers or revisions', () => {
  assert.throws(() => parseXml('<compounddef><broken></compounddef>'))
  assert.throws(() => renderReference(fixture(), 'main'), /commit hash/)
  assert.throws(() => renderReference([compound('<compounddef id="../escape" kind="class"><compoundname>Bad</compoundname></compounddef>')], revision), /identifier/)
})

test('removes stale generated pages', () => {
  mkdirSync(join(root, 'tmp'), { recursive: true })
  const output = mkdtempSync(join(root, 'tmp/compiler-api-test-'))
  try {
    writeReference(output, new Map([['old.mdx', 'Old symbol']]))
    writeReference(output, new Map([['new.mdx', 'New symbol']]))
    assert.deepEqual(readdirSync(output), ['new.mdx'])
    assert.equal(readFileSync(join(output, 'new.mdx'), 'utf8'), 'New symbol')
  } finally { rmSync(output, { recursive: true, force: true }) }
})

const doxygen = process.env.DOXYGEN || 'doxygen'
let available = false
try { available = execFileSync(doxygen, ['--version'], { encoding: 'utf8' }).startsWith('1.15.0') } catch {}

test('extracts real C++ declarations and definitions, static helpers and anonymous namespaces', { skip: !available }, async () => {
  mkdirSync(join(root, 'tmp'), { recursive: true })
  const work = mkdtempSync(join(root, 'tmp/compiler-api-extraction-'))
  try {
    writeFileSync(join(work, 'sample.h'), `
/** Provides test types. */
namespace sun::demo {
/** A base type. */
struct Base {};
/** Stores a value. */
class Box : public Base {
 public:
  /** Reads an integer. @param value The input. */
  int read(int value);
  /** Reads a boolean. */
  bool read(bool value);
  /** Available modes. */
  enum class Mode { Ready, Done };
  /** The stored type. */
  using Value = Base;
 private:
  int secret;
};
enum class Status { Ready };
using BoxPtr = Box*;
}
`)
    writeFileSync(join(work, 'sample.cpp'), `
#include "sample.h"
using sun::demo::Box;
namespace sun::deep::imports {
using sun::demo::Box;
using sun::demo::Status;
using sun::demo::BoxPtr;
}
int sun::demo::Box::read(int value) { return value; }
bool sun::demo::Box::read(bool value) { return value; }
namespace sun::access {
namespace {
/** Stores private helper state. */
struct State { int value; };
/** A file-local helper. */
static int helper() { int localOnly = 1; return localOnly; }
}
}
`)
    const config = readFileSync(join(root, 'docs/compiler-api/Doxyfile'), 'utf8')
    execFileSync(doxygen, ['-'], { cwd: root, input: `${config}\nINPUT = "${work}"\nOUTPUT_DIRECTORY = "${work}/out"\n`, stdio: ['pipe', 'pipe', 'pipe'] })
    const xmlDir = join(work, 'out/xml')
    const compounds = readdirSync(xmlDir).filter(f => f.endsWith('.xml') && f !== 'index.xml' && f !== 'Doxyfile.xml').flatMap(f => Array.from(parseXml(readFileSync(join(xmlDir, f), 'utf8')).getElementsByTagName('compounddef')))
    const pages = renderReference(compounds, revision)
    const all = [...pages.values()].join('\n')
    const box = [...pages.values()].find(p => p.startsWith('# sun::demo::Box\n'))
    assert.equal((box.match(/### read\n/g) || []).length, 2)
    assert.ok(!pages.has('classsun_1_1deep_1_1imports_1_1Box.mdx'))
    assert.ok(!pages.has('classBox.mdx'), 'using declarations must not create duplicate type pages')
    assert.doesNotMatch(pages.get('classes.mdx'), /File scope/)
    assert.match(box, /private · variable/)
    assert.ok(![...pages.keys()].some(name => name.startsWith('namespace') && name.includes('anonymous')))
    assert.match(all, /A file-local helper/)
    assert.doesNotMatch(pages.get('namespacesun_1_1access.mdx'), /anonymous/)
    assert.match(pages.get('functions.mdx'), />access</)
    assert.doesNotMatch(pages.get('enums.mdx'), /href="[^"]*imports#/)
    assert.doesNotMatch(pages.get('type-aliases.mdx'), /href="[^"]*imports#/)

    assert.ok([...pages.values()].some(page => page.startsWith('# sun::access::State\n')))
    assert.ok([...pages].some(([name, page]) => name.includes('sample_8cpp') && page.includes('### helper')))

    assert.match(all, /static/)
    assert.doesNotMatch(all, /localOnly/)
    checkLinks(pages)
    for (const [name, page] of pages) if (name.endsWith('.mdx')) await compile(page)
  } finally { rmSync(work, { recursive: true, force: true }) }
})
