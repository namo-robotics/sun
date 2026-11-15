/** Displays exception storage as vertically arranged stack and TLS memory tables. */
import type { CSSProperties } from 'react'

/** Identifies a snapshot in the nested-exception walkthrough. */
type Step = 1 | 2 | 3 | 4 | 5 | 6

/** Labels a source variable or illustrative runtime field and explains its current contents. */
type MemoryRow = {
  name: string
  type: string
  description: string
}

/** Groups consecutive memory rows under their owning frame or TLS record. */
type MemoryBlock = {
  name: string
  background: string
  heading: string
  rows: MemoryRow[]
}

const cell: CSSProperties = {
  padding: '8px 12px',
  border: '1px solid #94a3b8',
  textAlign: 'left',
  color: '#0f172a',
  lineHeight: 1.5,
}

/** Renders contiguous rows with one color per owner, including an explicit header. */
function MemoryColumn({ label, blocks }: { label: string; blocks: MemoryBlock[] }) {
  return <table aria-label={label} style={{ width: '100%', margin: 0, display: 'table', borderCollapse: 'collapse' }}>
    {blocks.map(block => <tbody key={block.name}>
      <tr><th scope="rowgroup" style={{ ...cell, background: block.heading, fontWeight: 700 }} colSpan={2}>{block.name}</th></tr>
      {block.rows.map(row => <tr key={row.name}>
        <th scope="row" style={{ ...cell, background: block.background, verticalAlign: 'top', fontWeight: 400, whiteSpace: 'nowrap', width: '1%' }}>
          <code style={{ whiteSpace: 'nowrap' }}>{row.name}: {row.type}</code>
        </th>
        <td style={{ ...cell, background: block.background, verticalAlign: 'top' }}>{row.description}</td>
      </tr>)}
    </tbody>)}
  </table>
}

/** Shows live frames and reserved exception values at one step of exception handling. */
export function ExceptionMemory({ step }: { step: Step }) {
  const nested = step >= 2 && step <= 4
  const stack: MemoryBlock[] = [{
    name: 'main() frame', background: '#dbeafe', heading: '#93c5fd',
    rows: [{
      name: 'error', type: 'A',
      description: step === 6 ? 'Caught exception A.' : 'Reserved catch storage; empty.',
    }],
  }]
  if (step !== 6) stack.push({
    name: 'foo() frame', background: '#dcfce7', heading: '#86efac',
    rows: [{ name: 'bar', type: 'Bar', description: step === 5 ? 'Destroyed; its storage remains until foo exits.' : 'Local resource owned by foo; destroyed during unwinding.' }],
  })
  if (nested) stack.push({
    name: 'Bar.deinit() frame', background: '#fce7f3', heading: '#f9a8d4',
    rows: [{
      name: 'previousTarget', type: 'PropagationRecordHeader*',
      description: 'Saved pointer to the TLS base record holding A.',
    }, {
      name: 'nested', type: 'Propagation storage',
      description: step === 3 ? 'Owns B during native propagation.' : 'Reserved propagation storage; empty and selected for nested throws.',
    }, {
      name: 'error', type: 'B',
      description: step === 4 ? 'Caught exception B.' : 'Reserved catch storage; empty.',
    }],
  })
  const tls: MemoryBlock[] = [{
    name: 'Thread exception state', background: '#fef3c7', heading: '#fcd34d',
    rows: [{
      name: 'state.throwTarget', type: 'PropagationRecordHeader*',
      description: nested ? 'Points to nested in the Bar.deinit() frame.' : 'Points to state.base in TLS.',
    }, {
      name: 'state.base', type: 'Propagation storage',
      description: step === 6 ? 'Reserved base storage; empty after transfer to main’s catch.' : 'Owns A while it propagates, including while cleanup handles B.',
    }],
  }]
  return <figure aria-label={`Step ${step}: stack and TLS memory`} style={{ margin: '24px 0' }}>
    <div style={{ marginBottom: 24 }}>
      <div style={{ fontWeight: 700, marginBottom: 8 }}>Stack</div>
      <div style={{ overflowX: 'auto' }}>
        <MemoryColumn label="Stack memory" blocks={stack} />
      </div>
    </div>
    <div>
      <div style={{ fontWeight: 700, marginBottom: 8 }}>Thread-local storage</div>
      <div style={{ overflowX: 'auto' }}>
        <MemoryColumn label="Thread-local memory" blocks={tls} />
      </div>
    </div>
  </figure>
}

/** Expands the runtime types separately from the live memory snapshots. */
export function ExceptionStorageTypes() {
  const blocks: MemoryBlock[] = [{
    name: 'Thread storage', background: '#fef3c7', heading: '#fcd34d',
    rows: [
      { name: 'throwTarget', type: 'PropagationRecordHeader*', description: 'Selects base or a destructor’s stack record for the next throw.' },
      { name: 'base', type: 'Propagation storage', description: 'Depth-sized base record reserved after the aligned header, once per thread.' },
    ],
  }, {
    name: 'Propagation storage', background: '#dbeafe', heading: '#93c5fd',
    rows: [
      { name: 'unwind', type: '_Unwind_Exception', description: 'Native unwind header; remains at a stable address during propagation.' },
      { name: 'value', type: 'ExceptionValueHeader', description: 'Fixed payload and trace metadata; populated frames follow this header.' },
      { name: 'frames', type: 'InstructionAddress[depth]', description: 'Trailing storage reserved by generated code. Catch storage has the same value header and frames, without an unwind header.' },
    ],
  }, {
    name: 'ExceptionValueHeader', background: '#dcfce7', heading: '#86efac',
    rows: [
      { name: 'type', type: 'ExceptionTypeDescriptor*', description: 'Identifies the concrete exception; null denotes a consumed value.' },
      { name: 'payload', type: 'byte[128]', description: 'Bounded user fields, separate from trace capacity.' },
      { name: 'trace', type: 'StackTraceHeader', description: 'Inline trace captured on the first throw and preserved on rethrow.' },
    ],
  }, {
    name: 'StackTraceHeader', background: '#fce7f3', heading: '#f9a8d4',
    rows: [
      { name: 'count', type: 'FrameCount', description: 'Number of valid stored addresses.' },
      { name: 'status', type: 'TraceStatus', description: 'Complete, truncated, partial, unavailable, or disabled. Inspect one extra frame to detect truncation.' },
    ],
  }]
  return <figure aria-label="Exception storage types" style={{ margin: '24px 0' }}>
    {blocks.map(block => <div key={block.name} style={{ overflowX: 'auto', marginBottom: 16 }}>
      <MemoryColumn label={`${block.name} fields`} blocks={[block]} />
    </div>)}
  </figure>
}
