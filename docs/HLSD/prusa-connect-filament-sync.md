# Prusa Connect filament synchronization

OrcaSlicer synchronizes Prusa Connect FFF tool metadata through the existing AMS
synchronization path. The sidebar's filament-sync action detects a Prusa Connect
physical printer, obtains the authenticated printer list and selected printer's
FFF GraphQL data, and converts each ordered Connect tool into an AMS tray
configuration. Empty tools are retained as placeholders, material IDs and
material types are passed to the normal preset matcher, and `colorRgba` values
are normalized to Orca's `#RRGGBB` representation. `PresetBundle::sync_ams_list`
then performs the same compatibility fallback, project filament update,
thumbnail/UI refresh, dirty-state handling, and synchronization badges used by
other AMS-capable agents.

The Device-tab WebView is the authentication boundary. Connect's OAuth/PKCE
session remains in the WebView; native code does not receive, persist, or log
passwords, access tokens, or refresh tokens. A narrowly scoped script executes
requests only while the current document is `https://connect.prusa3d.com`, reads
the access token from the WebView's authenticated storage, filters
credential-looking response fields, and sends only a response body/status
envelope through the existing wx script-message bridge. Native
request generation, expected operation kind, document navigation, reset, and
shutdown state prevent stale or unsolicited responses from reaching the GUI.
The WebView must not receive Orca's printer API-key injection: Connect OAuth
requests reject that cross-origin header and otherwise fail their CORS
preflight.

Printer and organization UUIDs are stored as connection metadata in the
physical-printer preset, not in project filament data. The selected printer can
be changed from the synchronization action; the physical-printer dialog also
exposes these identifiers for troubleshooting. No global filament profile is
modified automatically; project mutation is implemented by the native
synchronization path.

Connect's GraphQL schema and `/app/printers` endpoint are private APIs. The
parser accepts known wrapper and field variants, rejects malformed/error
responses, preserves ambiguous tool order rather than guessing, and reports
request/schema failures to the user. A future Connect schema change may still
require an update to the query or parser.
