# Hosting the U8 Ether Gate web build

This folder is a complete static site. Copy all of it, as it is, into any folder of any static web
host (any path works; nothing depends on the address) and open `index.html` through http(s).
Opening the file directly from disk (`file://`) doesn't work; for a test on your own machine run
`python3 -m http.server 8000` in this folder and open http://localhost:8000/.

- It contains none of Ultima VIII's files. Each player chooses their own copy; it is read in their
  browser, kept there (IndexedDB) and never sent anywhere. There is no server part: no accounts, no
  uploads, no cookies. Saves, settings and world-editor work stay in the browser too; the editor's
  Download / Upload make copies.
- HTTPS is recommended: browsers keep Cache Storage only on https:// or localhost, so on plain http
  the program's data is downloaded again at each visit.
- Nothing special is needed from the server. `.part` files can have any content type. Gzip is optional.
- Updating: replace the whole folder. Players get a notice when the version in `version.json` changes,
  and download only the parts that changed.
- The licence (GNU GPL v3, `LICENSE.txt`): if you put this on a site for others, you also have to offer
  the matching source. The simplest way is to put `U8-Ether-Gate-<version>-source.zip` beside it with a
  link from your page, unchanged.
