# Hermes Voice Agent, explained simply

This page explains the project in plain words, as if to a curious 10-year-old.
For the full details, read the [README](README.md).

## What is it?

Hermes Voice Agent is a talking helper you run on your own computer.

You can talk to it, or type to it, and it talks back. You can use it in two ways:

- **In a web browser** on your computer. This works today.
- **On the puck**, a small round gadget with a touch screen, a microphone and a
  speaker. The puck works, but it is still an experiment.

The helper has a face on screen. You can pick one of two characters:
**Zenti** or **Orbit**.

## Why does it exist?

Most voice helpers send what you say to a big company's computers. This one is
built to run on a computer you own, so your words stay with you. Nothing is
sent anywhere you didn't choose.

The bigger goal is to connect it to **Hermes**, an AI agent made by Nous Research.
Then one helper could follow you from your phone to the puck, and always ask
before it does anything important. That part is **not built yet**.

## How does it work?

Think of a relay race with four runners:

```text
You speak  ->  1. Ears     ->  2. Brain      ->  3. Mouth   ->  You hear the answer
               (speech to      (a chat AI       (text back
                text)           writes a         to speech)
                                reply)
```

1. **Ears:** a speech-to-text model turns your voice into words.
2. **Brain:** a chat model, running on your computer (for example through
   Ollama), writes an answer.
3. **Mouth:** a text-to-speech voice reads the answer out loud.

A small server program called the **voice gateway** passes the baton between
the runners. It also checks that only you can use it, with a password and a
private security certificate made just for your computer.

## What works today, and what doesn't

| Works today | Still an experiment | Not built yet |
| --- | --- | --- |
| Talking and typing in the browser | The puck gadget | Connecting to Hermes |
| Stop, mute and "new conversation" buttons | Battery and charging | Moving one chat between phone and puck |
| Saving a copy of your chat to a file | Updating the puck over Wi-Fi | Asking before the agent takes an action |

The helper forgets the conversation after 30 minutes of quiet, or when you press
**New conversation**. It does not record your voice to a file.

## What do you need?

- A computer running **Linux** (Ubuntu 24.04 or newer) on a normal Intel or AMD
  (x86-64) processor, with at least 8 GB of memory. Windows works through WSL2.
- **Docker**, which runs the gateway in a sealed box.
- **Ollama** (or another chat AI that speaks the OpenAI format) with a model
  downloaded.
- A speech-to-text model and a voice. You download these yourself; they are not
  included.

You do **not** need the puck. The browser is enough.

## Try it

**No setup at all:** the scripted demo shows the screens without any AI. It
needs only Python:

```bash
python3 tools/build_site.py
python3 -m http.server 8080 --bind 127.0.0.1 --directory dist/site
```

Then open <http://localhost:8080>. The answers are pre-written; nothing is
listening and nothing is sent anywhere.

**The real thing:** follow the
[step-by-step quick start in the README](README.md#run-the-real-browser-client-on-linux).
It takes you from downloading models to talking to the helper.

## Words you'll see

| Word | What it means |
| --- | --- |
| PWA | A website you can install like an app. |
| Gateway | The server program that connects the ears, brain and mouth. |
| STT | Speech to text: the "ears". |
| TTS | Text to speech: the "mouth". |
| Piper | The program that makes the voice. |
| faster-whisper | The program that listens and writes down your words. |
| Ollama | A program that runs chat AI models on your own computer. |
| TLS certificate | A digital ID card that proves the browser is talking to your gateway. |
| Puck | The round touch-screen gadget. |
| ESP32-S3 | The tiny computer chip inside the puck. |
| Hermes | The AI agent this project plans to connect to next. |

## What's next?

1. Connect the gateway straight to a Hermes agent.
2. Let one conversation move between your phone and the puck.
3. Show what the agent wants to do, and ask before it does anything important.
4. Make setup a guided, few-click process.

Want the details? Start with the [README](README.md), then
[how it is built](docs/ARCHITECTURE.md) and the
[Hermes plan](docs/HERMES-INTEGRATION.md).
