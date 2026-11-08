# Documentation and commit hygiene

- Do not commit session journals, test campaign reports, one-off measurements, or validation logs.
- Keep these results in Git-ignored `.cache/` files and summarize useful conclusions in the conversation.
- Update documentation only to explain necessary usage or maintenance procedures. Do not create new documents unless explicitly requested.
- Before every commit, review Markdown changes for reader value; remove descriptions of the work performed.
- Keep reproducible tests in the repository; keep their one-off results out of it.
