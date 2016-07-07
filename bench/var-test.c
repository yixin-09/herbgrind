#include <stdio.h>
#include <math.h>
#include "herbgrind.h"

double __attribute__ ((noinline)) add(double x, double y){
  return x + y;
}
int main(int argc, char** argv){
  volatile double x, y, z;
  HERBGRIND_BEGIN();
  z = 5;
  x = add(4, 5);
  y = add(6, z + 6);
  HERBGRIND_END();
}
