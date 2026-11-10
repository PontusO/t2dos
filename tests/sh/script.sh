# variables, arguments, $?, $((...)), if, while, for, test, quotes, patterns, redirection
name=world
echo hello $name "${name}s" '$name'
echo args: $# first=$1 second=$2 zero=$0
n=3
echo sum $((n * 2 + 1)) cmp $((n > 2)) $(( (n+1) % 3 )) neg $((-n + 1)) div $((7 / 2))
cat /nosuch
echo status $?
true; echo t $?; false; echo f $?
if [ $n -eq 3 ]; then echo n is 3; else echo n is not 3; fi
if [ "$name" = moon ]
then
  echo moon
else
  echo not moon
fi
i=0
while [ $i -lt 4 ]; do
  echo i=$i
  i=$((i + 1))
done
for f in a b "c d"; do echo item $f; done
for a; do echo arg $a; done
for f in /tmp/sh/*.SH; do echo file $f; done
for f in /tmp/sh/nomatch*; do echo literal $f; done
if [ -f /tmp/sh/script.sh ]; then echo script exists; fi
if [ ! -d /nosuchdir ]; then echo no dir; fi
if test -z "$empty"; then echo empty is empty; fi
if [ abc != abd ]; then echo differ; fi
x="a b"; echo "[$x]"
echo out > /tmp/sh/o.txt; echo more >> /tmp/sh/o.txt; cat /tmp/sh/o.txt
pwd > /tmp/sh/p.txt; cat /tmp/sh/p.txt
if true; then if false; then echo no; else echo nested ok; fi; fi
echo "a\"b" a\ b 'x  y' # a comment; echo no
exit 7
echo not reached
