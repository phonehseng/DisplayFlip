# DisplayFlip privacy

DisplayFlip is a personal Windows tool. It has no servers and collects nothing.

The optional `tv-google` hook signs in to your own Google account (scope: Google Assistant SDK) so it can
send text commands such as "turn on dorm TV" to Google on your behalf. The resulting sign-in token is stored
only on your PC, in `%APPDATA%\DisplayFlip\google-credentials.json`, and is sent only to Google when a command
runs. Delete that file, or remove DisplayFlip's access at https://myaccount.google.com/permissions, to revoke it.
