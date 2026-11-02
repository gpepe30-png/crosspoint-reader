# Trivia

Trivia is a button-only game using the published CrossPlay question pack. It
does not require a touchscreen.

## Controls

- **Ready:** Confirm starts a game; Previous opens settings; Next connects to
  Wi-Fi and checks reports and pack updates.
- **Game mode:** Previous/Next switches between Quizmaster and Solo; Confirm
  starts the selected mode.
- **Quizmaster:** Confirm reveals the answer; Next advances to another
  question.
- **Solo:** Previous/Next selects an answer; Confirm submits it. Confirm or
  Next advances after the answer is shown. The score appears when leaving.
- **Report a question:** Hold Confirm for about one second. The question is
  hidden locally; Previous/Next chooses a reason, Confirm saves it, and Back
  unhides the question and withdraws its unsent report.
- **Settings:** Previous/Next selects difficulty, US-centric questions, or
  report/update sync. Confirm changes a preference or starts Wi-Fi selection.
- **Pack update:** After sync reports an available update, Confirm downloads
  and installs it. Back keeps the installed pack.

Pack data, question flags, preferences, metadata, and queued reports are stored
on the SD card under `/trivia/`. Reports stay queued if submission fails. Pack
downloads are accepted only when their SHA-256 identity matches the verified
published manifest.

## TODO

- [ ] In Solo mode, give the selected answer a gray highlight that is easy to
  distinguish from the other three choices.
- [ ] Improve cluttered text layout and investigate whether the apparent font
  issue comes from wrapping/spacing or font rendering.
- [ ] Rework controls so the four bottom buttons select answers, Left goes Back,
  and Right submits; also consider double-pressing a selected bottom button to
  submit that answer.
