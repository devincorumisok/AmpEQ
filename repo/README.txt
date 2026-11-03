Sileo store page for AmpEQ (only needed if you host AmpEQ on your own repo).

1. Upload sileo-depiction.json and a banner.png (about 1500x500) to your repo, e.g. https://you.github.io/repo/ampeq/
2. In sileo-depiction.json, replace https://REPLACE-WITH-YOUR-REPO/ampeq/banner.png with the real banner link.
3. Add this line to the control file, then rebuild:
     SileoDepiction: https://you.github.io/repo/ampeq/sileo-depiction.json
Without a repo (installing the .deb directly), Sileo shows the Description from the control file instead.
