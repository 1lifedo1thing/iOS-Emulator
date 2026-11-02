# Native status snapshot

`Native.status()` preserves its eight string prefix entries: state, detail,
diagnostic, engine log path, QMP path, provider name, provider selection fixed,
and touch trace configured. Five entries follow in this fixed order:

| Index | Field |
| --- | --- |
| 8 | poll_snapshot_valid |
| 9 | guest_shutdown_confirmed |
| 10 | dirty |
| 11 | storage_failed |
| 12 | engine_exit |

All five describe the same existing `lt_runtime_poll` invocation under the JNI
control guard. Valid is `1` only with an active runtime and an `LT_OK` result.
Absent or failed polls have valid `0`; their initialized field values cannot
confirm shutdown. The state prefix retains its existing failure remapping.

A clean terminal receipt requires stopped state, valid `1`, confirmed `1`,
dirty `0`, storage failed `0`, and engine exit `0`. No extra observations,
engine phase poll, or public runtime API are introduced by these entries.

Owner-bound input cancellation adds indices13 (`displayInputGeneration`) and14
(`displayInputOwner`). The original thirteen entries keep their order and
meaning. These two values describe the guarded Android lease state, rather than
the runtime poll's shutdown facts; zero means no current owner/generation.
They do not acknowledge guest input delivery. Historical candidate22 still
contains the thirteen-entry producer and is unchanged by this source patch.

Index15 adds `vcpuAccountingConfigured`. It is `1` only after the debug-only
fresh-worker `debugArmVcpuAccounting()` successfully sets the fixed process
environment value `IT_VCPU_ACCT=5`. All fifteen prior entries retain their order;
the observations array and shared native ABI are unchanged. This flag describes
configuration, not actual accounting output or genuine iOS scheduler usage.
Ordinary workers never call this setter and report false. Release builds refuse
the operation without setting the environment.
