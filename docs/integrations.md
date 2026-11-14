# Account integrations (optional, planned)

The core assistant needs no accounts: appointments and notes are stored on the device.

Planned optional integrations, all authorized on the local network:

| Service | Method |
|---|---|
| Google Calendar | OAuth + PKCE via static relay page (`backend/relay/oauth-relay.html`) |
| Gmail / iCloud Mail | IMAP/SMTP with app passwords |
| iCloud Calendar | CalDAV with app-specific password |
| Outlook / Microsoft 365 | OAuth device code flow |

Tokens are stored in encrypted NVS. Enable flash encryption + Secure Boot before storing real credentials.
