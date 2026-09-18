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
    ['main()', '/compiler-api/main', ''],
  ], namespaces, refs)
  assert.match(symbols, /Expr&#60;T&#62;/)
  assert.match(symbols, /<TreeSummary>File scope<\/TreeSummary>/)
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
