"""String-escape smoke, Python driver (#756); run by run-string-escapes.sh."""
import os

import orly

VALUES = [
    "plain",
    'quote " and backslash \\',
    "a\nb",
    "a\tb",
    "a\rb",
    "a\fb",
    "a\x01b",
    "a\x1fb",
    "a\x7fb",
    "line one\r\nline two\n\n\ttabbed",
    "literal backslash-n: a\\nb",
]

c = orly.connect(os.environ["ORLY_URL"], timeout=10, recv_timeout=60)
c.new_session()
c.install("string_escapes", 1)
pov = c.new_pov()

for n, value in enumerate(VALUES):
    c.call(pov, "string_escapes", "ps", {"k": n, "s": value})
    got = c.call(pov, "string_escapes", "gs", {"k": n})
    assert got == value, "py: wrote %r, read back %r" % (value, got)
print("py: %d strings round-tripped" % len(VALUES))
c.close()
