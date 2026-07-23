# madc grammar

`madc.ebnf` documents the surface language madc accepts, in the W3C EBNF
notation consumed by the
[Railroad Diagram Generator](https://github.com/GuntherRademacher/rr) (rr).
The hand-written parser (`src/parser.cpp`) is authoritative; this grammar
is descriptive and refreshed at release milestones.

A rendered, navigable railroad-diagram version lives in the
[mingodad/cpp-grammars](https://github.com/mingodad/cpp-grammars) collection
(requested in issue [#6](https://github.com/derekbsnider/madc/issues/6)).

## Regenerating the diagrams

```bash
# rr-webapp from Maven Central (needs a JRE):
curl -sL -o rr.war \
  https://repo1.maven.org/maven2/de/bottlecaps/rr/rr-webapp/2.6/rr-webapp-2.6.war
java -jar rr.war -out:madc.ebnf.xhtml docs/grammar/madc.ebnf
```

The generated `madc.ebnf.xhtml` is not checked in here — the `.ebnf` is the
source of truth.
