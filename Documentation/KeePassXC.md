# KeePassXC in Ladybird

In the Linux Qt browser, focus a sign-in field on an HTTPS page and press **Ctrl+Shift+L**.
Review the destination and choose an account. Ladybird fills the eligible fields in the
top-level document, and you can submit the form after reviewing it.

The Ladybird menu also offers **Fill with KeePassXC** and **Save or Update in KeePassXC…**.
Saving opens a browser dialog for the username and password, with the destination shown
as read-only. Updating asks you to choose the existing account first. KeePassXC may ask
for confirmation before it writes the entry.

Enable browser integration in KeePassXC. The first request asks you to associate the
database with Ladybird. The next request can prompt you to unlock the database. Ladybird
uses `/usr/bin/keepassxc-proxy`, Python 3 and libsodium for the encrypted native messaging
protocol, and stores the per-profile association in the session keyring with Secret Service.

KeePassXC applies its database's URL matching rules to the origin shown in Ladybird's
account chooser. The browser requires a focused field, a regular HTTPS tab and a
same-origin form destination. For a sign-in page with multiple password fields,
focus the intended password field. On a username-only step, the focused input must
declare `autocomplete="username"`. Choose the account again on the password step.

The bridge can be checked without a real database with
`python3 -B Tests/UI/Qt/test-keepassxc-bridge.py`.
