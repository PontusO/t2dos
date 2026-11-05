// undef.c - calls a function no file defines: cc compiles it, ld reports it
int main() {
  return nowhere(1);
}
