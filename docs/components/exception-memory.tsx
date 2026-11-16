/** Displays exception storage as vertically arranged stack and TLS memory tables. */
import type { CSSProperties } from "react";

/** Identifies a snapshot in the nested-exception walkthrough. */
type Step = 1 | 2 | 3 | 4 | 5 | 6;

/** Labels a source variable or illustrative runtime field and explains its current contents. */
type MemoryRow = {
  name: string;
  description: string;
};

/** Groups consecutive memory rows under their owning frame or TLS record. */
type MemoryBlock = {
  name: string;
  background: string;
  heading: string;
  rows: MemoryRow[];
};

const cell: CSSProperties = {
  padding: "8px 12px",
  border: "1px solid #94a3b8",
  textAlign: "left",
  color: "#0f172a",
  lineHeight: 1.5,
};

/** Renders contiguous rows with one color per owner, including an explicit header. */
function MemoryColumn({
  label,
  blocks,
}: {
  label: string;
  blocks: MemoryBlock[];
}) {
  return (
    <table
      aria-label={label}
      style={{
        width: "100%",
        margin: 0,
        display: "table",
        borderCollapse: "collapse",
      }}
    >
      {blocks.map((block) => (
        <tbody key={block.name}>
          <tr>
            <th
              scope="rowgroup"
              style={{ ...cell, background: block.heading, fontWeight: 700 }}
              colSpan={2}
            >
              {block.name}
            </th>
          </tr>
          {block.rows.map((row) => (
            <tr key={row.name}>
              <th
                scope="row"
                style={{
                  ...cell,
                  background: block.background,
                  verticalAlign: "top",
                  fontWeight: 400,
                  whiteSpace: "nowrap",
                  width: "1%",
                }}
              >
                <code style={{ whiteSpace: "nowrap" }}>{row.name}</code>
              </th>
              <td
                style={{
                  ...cell,
                  background: block.background,
                  verticalAlign: "top",
                }}
              >
                {row.description}
              </td>
            </tr>
          ))}
        </tbody>
      ))}
    </table>
  );
}

/** Stores independently editable stack and thread-local rows for each walkthrough step. */
const memorySteps: Record<Step, { stack: MemoryBlock[]; tls: MemoryBlock[] }> =
  {
    1: {
      stack: [
        {
          name: "main() frame",
          background: "#dbeafe",
          heading: "#93c5fd",
          rows: [
            {
              name: "error: A",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
        {
          name: "foo() frame",
          background: "#dcfce7",
          heading: "#86efac",
          rows: [
            {
              name: "bar: Bar",
              description:
                "Local resource owned by foo; destroyed during unwinding.",
            },
          ],
        },
      ],
      tls: [
        {
          name: "Thread exception state",
          background: "#fef3c7",
          heading: "#fcd34d",
          rows: [
            {
              name: "baseException: PropagationRecord",
              description:
                "Owns A while it propagates, including while cleanup handles B.",
            },
            {
              name: "throwTarget: PropagationRecord*",
              description: "Pointer to baseException",
            },
          ],
        },
      ],
    },
    2: {
      stack: [
        {
          name: "main() frame",
          background: "#dbeafe",
          heading: "#93c5fd",
          rows: [
            {
              name: "error: A",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
        {
          name: "foo() frame",
          background: "#dcfce7",
          heading: "#86efac",
          rows: [
            {
              name: "bar: Bar",
              description:
                "Local resource owned by foo; destroyed during unwinding.",
            },
          ],
        },
        {
          name: "Bar.deinit() frame",
          background: "#fce7f3",
          heading: "#f9a8d4",
          rows: [
            {
              name: "previousTarget: PropagationRecord*",
              description: "Saved pointer to the TLS base record holding A.",
            },
            {
              name: "nested: PropagationRecord",
              description:
                "Reserved propagation storage; empty and selected for nested throws.",
            },
            {
              name: "error: B",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
      ],
      tls: [
        {
          name: "Thread exception state",
          background: "#fef3c7",
          heading: "#fcd34d",
          rows: [
            {
              name: "baseException: PropagationRecord",
              description: "Owns A while the destructor prepares to handle nested throws.",
            },
            {
              name: "throwTarget: PropagationRecord*",
              description: "Points to nested in the Bar.deinit() frame.",
            },
          ],
        },
      ],
    },
    3: {
      stack: [
        {
          name: "main() frame",
          background: "#dbeafe",
          heading: "#93c5fd",
          rows: [
            {
              name: "error: A",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
        {
          name: "foo() frame",
          background: "#dcfce7",
          heading: "#86efac",
          rows: [
            {
              name: "bar: Bar",
              description:
                "Local resource owned by foo; destroyed during unwinding.",
            },
          ],
        },
        {
          name: "Bar.deinit() frame",
          background: "#fce7f3",
          heading: "#f9a8d4",
          rows: [
            {
              name: "previousTarget: PropagationRecord*",
              description: "Saved pointer to the TLS base record holding A.",
            },
            {
              name: "nested: PropagationRecord",
              description: "Owns B during native propagation.",
            },
            {
              name: "error: B",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
      ],
      tls: [
        {
          name: "Thread exception state",
          background: "#fef3c7",
          heading: "#fcd34d",
          rows: [
            {
              name: "baseException: PropagationRecord",
              description:
                "Owns A while it propagates, including while cleanup handles B.",
            },
            {
              name: "throwTarget: PropagationRecord*",
              description: "Points to nested in the Bar.deinit() frame.",
            },
          ],
        },
      ],
    },
    4: {
      stack: [
        {
          name: "main() frame",
          background: "#dbeafe",
          heading: "#93c5fd",
          rows: [
            {
              name: "error: A",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
        {
          name: "foo() frame",
          background: "#dcfce7",
          heading: "#86efac",
          rows: [
            {
              name: "bar: Bar",
              description:
                "Local resource owned by foo; destroyed during unwinding.",
            },
          ],
        },
        {
          name: "Bar.deinit() frame",
          background: "#fce7f3",
          heading: "#f9a8d4",
          rows: [
            {
              name: "previousTarget: PropagationRecord*",
              description: "Saved pointer to the TLS base record holding A.",
            },
            {
              name: "nested: PropagationRecord",
              description:
                "Reserved propagation storage; empty and selected for nested throws.",
            },
            {
              name: "error: B",
              description: "Caught exception B.",
            },
          ],
        },
      ],
      tls: [
        {
          name: "Thread exception state",
          background: "#fef3c7",
          heading: "#fcd34d",
          rows: [
            {
              name: "baseException: PropagationRecord",
              description:
                "Owns A while it propagates, including while cleanup handles B.",
            },
            {
              name: "throwTarget: PropagationRecord*",
              description: "Points to nested in the Bar.deinit() frame.",
            },
          ],
        },
      ],
    },
    5: {
      stack: [
        {
          name: "main() frame",
          background: "#dbeafe",
          heading: "#93c5fd",
          rows: [
            {
              name: "error: A",
              description: "Reserved catch storage; empty.",
            },
          ],
        },
        {
          name: "foo() frame",
          background: "#dcfce7",
          heading: "#86efac",
          rows: [
            {
              name: "bar: Bar",
              description: "Destroyed; its storage remains until foo exits.",
            },
          ],
        },
      ],
      tls: [
        {
          name: "Thread exception state",
          background: "#fef3c7",
          heading: "#fcd34d",
          rows: [
            {
              name: "baseException: PropagationRecord",
              description:
                "Owns A while it propagates, including while cleanup handles B.",
            },
            {
              name: "throwTarget: PropagationRecord*",
              description: "Points to baseException",
            },
          ],
        },
      ],
    },
    6: {
      stack: [
        {
          name: "main() frame",
          background: "#dbeafe",
          heading: "#93c5fd",
          rows: [
            {
              name: "error: A",
              description: "Caught exception A.",
            },
          ],
        },
      ],
      tls: [
        {
          name: "Thread exception state",
          background: "#fef3c7",
          heading: "#fcd34d",
          rows: [
            {
              name: "baseException: PropagationRecord",
              description:
                "Reserved base storage; empty after transfer to main’s catch.",
            },
            {
              name: "throwTarget: PropagationRecord*",
              description: "Points to baseException",
            },
          ],
        },
      ],
    },
  };

/** Shows live frames and reserved exception values at one step of exception handling. */
export function ExceptionMemory({ step }: { step: Step }) {
  const { stack, tls } = memorySteps[step];
  return (
    <figure
      aria-label={`Step ${step}: stack and TLS memory`}
      style={{ margin: "24px 0" }}
    >
      <div style={{ marginBottom: 24 }}>
        <div style={{ fontWeight: 700, marginBottom: 8 }}>Stack</div>
        <div style={{ overflowX: "auto" }}>
          <MemoryColumn label="Stack memory" blocks={stack} />
        </div>
      </div>
      <div>
        <div style={{ fontWeight: 700, marginBottom: 8 }}>
          Thread-local storage
        </div>
        <div style={{ overflowX: "auto" }}>
          <MemoryColumn label="Thread-local memory" blocks={tls} />
        </div>
      </div>
    </figure>
  );
}

/** Expands the runtime types separately from the live memory snapshots. */
export function ExceptionStorageTypes() {
  const blocks: MemoryBlock[] = [
    {
      name: "ThreadExceptionState",
      background: "#fef3c7",
      heading: "#fcd34d",
      rows: [
        {
          name: "baseException: PropagationRecord",
          description:
            "Depth-sized base record reserved after the aligned header, once per thread.",
        },
        {
          name: "throwTarget: PropagationRecord*",
          description:
            "Selects baseException or a destructor’s stack record for the next throw.",
        },
      ],
    },
    {
      name: "PropagationRecord",
      background: "#dbeafe",
      heading: "#93c5fd",
      rows: [
        {
          name: "unwind",
          description:
            "Native unwind header; remains at a stable address during propagation.",
        },
        {
          name: "value",
          description:
            "Exception type, payload, and inline stack trace. Catch storage holds this value without an unwind header.",
        },
      ],
    },
    {
      name: "ExceptionValue",
      background: "#dcfce7",
      heading: "#86efac",
      rows: [
        {
          name: "type",
          description:
            "Identifies the concrete exception; null denotes a consumed value.",
        },
        {
          name: "payload",
          description: "Bounded user fields, separate from trace capacity.",
        },
        {
          name: "trace",
          description:
            "Inline trace captured on the first throw and preserved on rethrow.",
        },
      ],
    },
    {
      name: "StackTrace",
      background: "#fce7f3",
      heading: "#f9a8d4",
      rows: [
        {
          name: "frames",
          description:
            "Inline address array whose length is the configured trace depth.",
        },
        { name: "count", description: "Number of valid stored addresses." },
        {
          name: "status",
          description:
            "Complete, truncated, partial, unavailable, or disabled. Inspect one extra frame to detect truncation.",
        },
      ],
    },
  ];
  return (
    <figure aria-label="Exception storage types" style={{ margin: "24px 0" }}>
      {blocks.map((block) => (
        <div key={block.name} style={{ overflowX: "auto", marginBottom: 16 }}>
          <MemoryColumn label={`${block.name} fields`} blocks={[block]} />
        </div>
      ))}
    </figure>
  );
}
