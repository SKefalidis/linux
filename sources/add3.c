#include <stdio.h>


int main(void) {
	unsigned long long a[100000];
	unsigned long long b[100000];
	unsigned long long c[100000];
	
	for (int j = 0; j < 100; j++) {
		for (int i = 0; i < 100; i++) {
			a[i] = i;
			b[i] = 100 - i;
		}
	}
	
	for (unsigned long long k = 0; k < 100000; k++) {
		for (unsigned long long j = 0; j < 100000; j += 100) {
			for (unsigned long long i = 0; i < 100000; i += 100) {
				a[i] = a[i] + k;
				c[i] = a[i] * b[i] + a[j] + b[j];
			}
		}
	}
}
