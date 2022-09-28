#include <stdio.h>
#include <unistd.h>

#define PRIME_COUNT 1000


int main (void) {
    
    while (1) {
        unsigned int array[PRIME_COUNT];
        unsigned int current = 2;

        sleep(3);
        
        for (int i = 0; i < PRIME_COUNT; i++) {
            while (1) {
                int is_prime = 1;
                for (int j = 2; j < current; j++) {
                    if (current % j == 0) {
                        is_prime = 0;
                    }
                }
                if (is_prime == 0) {
                    current++;
                } else {
                    break;
                }
            }
            array[i] = current;
            current++;
        }
    }

    return 0;
}