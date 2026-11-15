# Writes romfs/cacert.pem: the root certificates kahoot.it chains to (Amazon Root CA 1-4, Starfield Services Root G2),
# plus Let's Encrypt and DigiCert/GlobalSign roots in case Kahoot changes CDN. Taken from certifi.
import certifi, re, sys
WANT = ["Amazon Root CA 1", "Amazon Root CA 2", "Amazon Root CA 3", "Amazon Root CA 4",
        "Starfield Services Root Certificate Authority - G2", "ISRG Root X1", "ISRG Root X2",
        "DigiCert Global Root G2", "GTS Root R1", "GTS Root R4"]
text = open(certifi.where(), encoding="utf-8").read()
out, found = [], set()
for label, pem in re.findall(r"# Label: \"([^\"]+)\".*?(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)", text, re.S):
    if label in WANT: out.append(f"# {label}\n{pem}\n"); found.add(label)
missing = set(WANT) - found
if missing: print("missing:", missing)
open(sys.argv[1] if len(sys.argv) > 1 else "romfs/cacert.pem", "w", newline="\n").write("".join(out))
print(len(out), "certs")
