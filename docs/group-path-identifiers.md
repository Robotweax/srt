# Optional group path identifiers

This development-branch extension is absent from the published v0.2.8 artifacts.
It adds receiver-visible caller labels to Live-mode Broadcast and Backup groups
using Caller/Listener setup. Both peers must support and explicitly enable it;
it is disabled by default. Enabled connections are not Haivision-compatible.
There is no automatic capability discovery or fallback to an unlabeled member.

## Configure a labeled connection

Before the first caller member connects, set the boolean
`SRTO_ROBOTWEAX_PATHID_REQUIRED` (`0x01000006`) on its group. On the listener,
set both `SRTO_GROUPCONNECT` and `SRTO_ROBOTWEAX_PATHID_REQUIRED` before listen.
A required listener rejects ordinary connections and unlabeled groups. Use a
separate ordinary listener when accepting peers without this extension.

Attach a distinct label to each endpoint's copied configuration using
`SRTO_ROBOTWEAX_PATHID` (`0x01000007`). Labels are 1–32 opaque bytes; embedded NUL
is allowed and length is authoritative. No text normalization is performed.

```c
bool required = true;
if (srt_setsockflag(group, SRTO_ROBOTWEAX_PATHID_REQUIRED,
                   &required, sizeof(required)) == SRT_ERROR) {
    /* This provider may not implement the extension. */
}
SRT_SOCKOPT_CONFIG* options = srt_create_config();
const unsigned char label[] = {'w', 'a', 'n', '-', 'a'};
if (!options || srt_config_add(options, SRTO_ROBOTWEAX_PATHID,
                              label, sizeof(label)) == SRT_ERROR) {
    /* Handle configuration failure. */
}
/* Assign options to the appropriate SRT_SOCKGROUPCONFIG.config before
   srt_connect_group. Delete it with srt_delete_config when no longer needed. */
```

The endpoint snapshot owns its bytes. An empty value clears a label before
setup. A missing label in required mode, or a label with required mode disabled,
is rejected before connecting, with `SRT_EINVPARAM` in the endpoint’s
`errorcode`. If no member starts, the group call returns `SRT_ECONNSETUP`.
Setting a label on a group handle returns
`SRT_EINVOP`; it has no group-wide default. Configure endpoint labels separately.
The required policy cannot change after the group has opened. Socket label and
policy setters cannot change an established, connecting or retained broken
member. Listen callbacks cannot change these options during admission.

Rendezvous, File/Control/Sensor groups and standalone labeled connections are
unsupported. Conflicting Rendezvous and transfer-profile setters fail in either
order. A listener must enable group admission before listening in required mode.
Existing passphrase, encryption, filter, authorization-domain and membership
rules still apply independently; a label grants no permission to join a group.

## Read metadata

The separate C symbol `robotweax_srt_group_path_data_v1` returns an array of
`ROBOTWEAX_SRT_GROUP_PATHDATA_V1` records. The v1 record has a fixed 44-byte layout:

| Field | Type | Meaning |
| --- | --- | --- |
| `member_id` | `int32_t` | Local member socket ID |
| `flags` | `uint32_t` | Value 1: local offer; value 2: completed negotiation |
| `identifier_length` | `uint32_t` | 0 for absence, otherwise 1–32 bytes |
| `identifier` | `uint8_t[32]` | Exact label with unused bytes zero-filled |

Use `ROBOTWEAX_SRT_PATHID_LOCAL_OFFER` and
`ROBOTWEAX_SRT_PATHID_NEGOTIATED` for the flag bits. Pending callers expose their
configured offer with the first flag alone. Successful caller members have both
flags; accepted members have the negotiated flag alone. Provisional listener
members may contain an offered label without either flag while setup completes.
Ordinary members have zero length, zero flags and zero-filled bytes.

The getter follows `srt_group_data`'s count and capacity convention. Pass null
output and a valid count pointer to query the required element count; success
returns zero. With output, the count supplies capacity and receives the number
of members; success returns that count. Insufficient capacity returns `SRT_ERROR`
and `SRT_ELARGEMSG`, updates the required count and leaves the array untouched.
Membership can change between query and copy, so retry that case. A null count,
invalid/non-group handle or closed group returns `SRT_EINVPARAM`. Every copy is
one coherent membership snapshot and requires no new per-member allocation.

Match member IDs to existing group data and per-member statistics. Existing
`SRT_SOCKGROUPDATA`, `SRT_SOCKGROUPCONFIG`, `SRT_MSGCTRL.grpdata` and callback
signatures are unchanged. `token` remains local application metadata; it is not
transmitted or replaced by a peer's label. Accepted tokens remain local values.
The new getter does not identify the member that delivered a deduplicated message.

The export is additive to the existing C ABI. Hosts that load interchangeable
providers must resolve this symbol from the selected Robotweax provider and
handle its absence on older libraries and other providers. Do not call a symbol
from another loaded library against this provider's handles. A future record
layout will use a new type and versioned symbol rather than change v1's stride.

## Reconnect and identity scope

The application chooses and reuses the bytes for a replacement of the same
logical path. Changing the source address or port does not change the label
when the replacement is configured with those bytes. A new socket does not
infer an old label. This adds no live NAT rebinding or transport migration.

Two overlapping members may use the same label during replacement. They remain
separate sockets with separate counters, keys, buffers and timing. The library
does not merge them, evict a member or change Backup send selection because their
labels match. Track both member IDs until the old member is retired; dashboards
can display the overlap explicitly.

A label persists on a retained BROKEN member until explicit close removes it.
Group close releases all its metadata. There is no permanent reconnect history.
Scope labels to an application-authorized peer/group association. Correlating a
wholly recreated group across sessions needs the application's session namespace;
peer-supplied group IDs and labels are not globally unique or authoritative.

## Private v1 handshake contract

The HSv5 CONFIG command is `0x7f11`. INDUCTION remains unchanged. A caller offers
one record in CONCLUSION alongside ordinary Group Membership; the listener
requires the enabled policy and echoes the label in its CONCLUSION response.
The caller requires an exact echo before completing setup. Unsupported or
unlabeled responses fail rather than silently downgrade. An older peer may
ignore the record, reject it or time out; enabled interoperability is not claimed.
Group calls retain their existing aggregate error behavior; per-member rejection
uses `SRT_REJ_GROUP` when this implementation detects a policy/echo mismatch.

Content is exactly 40 bytes (10 words), preceded by the ordinary four-byte
extension header:

| Offset in content | Bytes | Value |
| --- | --- | --- |
| 0 | 4 | Network-order uint32 version, exactly 1 |
| 4 | 4 | Network-order uint32 label length, 1–32 |
| 8 | 32 | Raw label bytes followed by zero padding |

Malformed sizes, versions, padding, duplicate records and missing CONFIG
advertisement fail decoding. Ordinary unlabeled mode does not emit this command.
A labeled handshake must fit the advertised MSS after reserving 48 bytes for an
IPv6/UDP envelope; encoding fails rather than truncate or implicitly fragment.
StreamID, crypto, filter and other records share that same complete budget.

Offers and echoes are cached for retries. Changed labels cannot overwrite the
original transaction or relabel a connected member; inconsistent retries are
ignored. Session-authenticated setup repeats the same label in AGREEMENT and
requires consistency before publication. Ordinary offer/echo setup retains the
existing two-message limitation: it does not prove both applications observed
connection success. Labels do not change the standard Group Membership record,
DATA format, sequence space, replay, TSBPD or scheduler model.

## Trust and privacy

Labels are cleartext, caller-supplied telemetry metadata. Escape arbitrary bytes
before displaying them and do not store secrets in them. A label proves neither
physical interface identity nor independent network failure domains.

AES-CTR, AES-GCM DATA integrity and
[session authentication v2](session-authentication.md) do not authenticate this
handshake label. An active attacker or authorized peer may lie about it. Existing
group admission and security-domain checks remain the authorization boundary.
Authenticated path identity would require a separate transcript-authentication
contract; this feature makes no such claim.

See [Connection groups](connection-groups.md) for membership, state and retained
statistics, and [API compatibility](api-compatibility.md) for the C boundary.
