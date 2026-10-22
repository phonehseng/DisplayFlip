"""DisplayFlip hook: control a Google Home device (like a Google TV) with the same text commands you
could say to Google ("turn off dorm TV"). Commands go through Google's servers, so this works even when
the PC and the TV can't see each other on the local network (campus Wi-Fi, VPN...).

  tv-google.cmd setup                  one-time Google sign-in (needs client_secret.json in this folder)
  tv-google.cmd on dorm TV             -> "turn on dorm TV"
  tv-google.cmd off dorm TV            -> "turn off dorm TV"
  tv-google.cmd say is dorm TV on      -> any other command, prints Google's reply

settings (config.json):  "onTurnOn": ["tv-google.cmd on dorm TV"], "onTurnOff": ["tv-google.cmd off dorm TV"]
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CLIENT = os.path.join(HERE, "client_secret.json")
CREDS = os.path.join(HERE, "google-credentials.json")
SCOPE = "https://www.googleapis.com/auth/assistant-sdk-prototype"


def setup():
    if not os.path.exists(CLIENT):
        sys.exit("client_secret.json not found in " + HERE + " (download it from Google Cloud, see README)")
    from google_auth_oauthlib.flow import InstalledAppFlow

    flow = InstalledAppFlow.from_client_secrets_file(CLIENT, scopes=[SCOPE])
    creds = flow.run_local_server(port=0, prompt="consent", open_browser=True)
    with open(CREDS, "w") as f:
        json.dump({"refresh_token": creds.refresh_token, "token_uri": creds.token_uri,
                   "client_id": creds.client_id, "client_secret": creds.client_secret, "scopes": [SCOPE]}, f)
    print("Signed in. Saved", CREDS)


def say(text):
    if not os.path.exists(CREDS):
        sys.exit("Not signed in yet: run  tv-google.cmd setup")
    import google.oauth2.credentials
    from gassist_text import TextAssistant

    with open(CREDS) as f:
        creds = google.oauth2.credentials.Credentials(token=None, **json.load(f))
    with TextAssistant(creds, language_code="en-US") as assistant:
        reply = assistant.assist(text)
    print(f"> {text}\n{reply[0] or '(done, no text reply)'}")


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__)
        return
    verb, rest = argv[0].lower(), " ".join(argv[1:]).strip()
    if verb == "setup":
        setup()
    elif verb in ("on", "off"):
        device = rest or os.environ.get("DISPLAYFLIP_DISPLAY", "TV")
        say(f"turn {verb} {device}")
    elif verb == "say":
        say(rest)
    else:
        say(" ".join(argv))


if __name__ == "__main__":
    main(sys.argv[1:])
