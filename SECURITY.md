# Security

JLang runs untrusted programs in the browser playground and is built into device firmware, so a bug that lets a JLang program read or write memory outside the VM matters. If you find one, please report it privately through GitHub's [private vulnerability reporting](https://github.com/jafarnz/JLang/security/advisories/new) rather than in a public issue. You'll get a reply within a week, and credit in the fix if you'd like it.

`make sanitize` runs the tests under AddressSanitizer and UndefinedBehaviorSanitizer, and is the first tool to reach for when checking a fix.
