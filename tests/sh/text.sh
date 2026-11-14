# text.sh: grep, wc, head and tail on small files, missing files and usage errors
echo alpha one > /tmp/sh/t1.txt
echo Beta two >> /tmp/sh/t1.txt
echo gamma three >> /tmp/sh/t1.txt
echo delta >> /tmp/sh/t1.txt
echo beta again > /tmp/sh/t2.txt
grep beta /tmp/sh/t1.txt
echo plain $?
grep -in beta /tmp/sh/t1.txt /tmp/sh/t2.txt
grep -c a /tmp/sh/t1.txt
grep -v a /tmp/sh/t1.txt
echo inverted $?
grep "^.e.*o$" /tmp/sh/t1.txt
grep zzz /tmp/sh/t1.txt
echo none $?
grep x /tmp/sh/nosuch.txt
echo missing $?
grep x
echo usage $?
wc /tmp/sh/t1.txt
wc -l /tmp/sh/t1.txt /tmp/sh/t2.txt
wc /tmp/sh/nosuch.txt
echo wcmissing $?
head -n 2 /tmp/sh/t1.txt
head -1 /tmp/sh/t1.txt /tmp/sh/t2.txt
tail -n 2 /tmp/sh/t1.txt
tail -1 /tmp/sh/t1.txt /tmp/sh/t2.txt
tail -n 0 /tmp/sh/t1.txt
tail /tmp/sh
echo taildir $?
