/** Checks that grouped ABI examples publish the exact sources exercised by CI. */
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'
import { readFileSync, readdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import test from 'node:test'
import { compile } from '@mdx-js/mdx'

const docs = join(dirname(fileURLToPath(import.meta.url)), '..')
const root = join(docs, '..')

/** Grouped examples must retain their own configs and both consumer cases. */
test('ABI documentation includes each project config and its exact Sun sources', async () => {
  execFileSync(process.execPath, [join(docs, 'scripts/gen-examples.mjs')])
  const host = readFileSync(join(docs, 'pages/abi/examples.mdx'), 'utf8')
  await compile(host)
  for (const name of ['target', 'identity', 'generics']) {
    const directory = join(root, 'examples/abi', name)
    const partial = `abi-${name}.mdx`
    assert.ok(host.includes(`generated/${partial}`), `Missing import for ${name}`)
    const rendered = readFileSync(join(docs, 'generated', partial), 'utf8')
    await compile(rendered)
    assert.ok(rendered.includes(`examples/abi/${name}/`))
    for (const file of readdirSync(directory).filter(file => file.endsWith('.sun') || file === 'sun-config.json')) {
      const source = readFileSync(join(directory, file), 'utf8').trimEnd()
      assert.ok(rendered.includes(`filename="${file}"\n${source}\n`), `Missing source: ${name}/${file}`)
    }
  }
})
