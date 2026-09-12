set -euo pipefail
closure () {
  echo "warn $1" >&2 || true
  if [ "$1" = "B" ]; then printf 'x\n' | grep 'ucrt64'; else printf '/ucrt64/bin/lib%s.dll\n' "$1"; fi
}
{
  for f in A B C D; do closure "$f"; done
} | sort -u | while read -r dll; do echo "COPY $dll"; done
echo "script exit code path reached"
