#include <stdio.h>


int main(void) {
	unsigned long long a[10000];
	unsigned long long b[10000];
	unsigned long long c[10000];
	
	for (int j = 0; j < 100; j++) {
		for (int i = 0; i < 100; i++) {
			a[i] = i;
			b[i] = 100 - i;
		}
	}
	
	for (int k = 0; k < 100000; k++) {
		for (int j = 0; j < 10000; j += 100) {
			for (int i = 0; i < 10000; i += 100) {
				a[i] = a[i] + k;
				c[i] = a[i] * b[i] + a[j] + b[j];
			}
		}
	}
}
