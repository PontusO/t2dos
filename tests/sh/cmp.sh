# cmp.sh: cmp on equal, different, shorter, missing files and a directory
echo one > /tmp/sh/c1.txt
echo two >> /tmp/sh/c1.txt
cp /tmp/sh/c1.txt /tmp/sh/c2.txt
cmp /tmp/sh/c1.txt /tmp/sh/c2.txt
echo same $?
echo one > /tmp/sh/c2.txt
echo twO >> /tmp/sh/c2.txt
cmp /tmp/sh/c1.txt /tmp/sh/c2.txt
echo differ $?
echo one > /tmp/sh/c2.txt
cmp /tmp/sh/c1.txt /tmp/sh/c2.txt
echo shorter $?
cmp -s /tmp/sh/c1.txt /tmp/sh/c2.txt
echo quiet $?
cmp /tmp/sh/c1.txt /tmp/sh/nosuch.txt
echo missing $?
cmp /tmp/sh /tmp/sh/c1.txt
echo dir $?
cmp /tmp/sh/c1.txt
echo usage $?
