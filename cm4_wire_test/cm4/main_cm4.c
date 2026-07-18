/* cm4_wire_test CM4 firmware — STUB (Task 1). No I2C yet: the gate must FAIL
 * (RED) until Task 2 fills in the self-config + transactions + MU stream. */
#include <stdint.h>

/* The shared vector table (startup_cm4.S) references these C symbols. */
void SysTick_Handler(void) {}
void MU_IRQHandler(void) {}

int main(void)
{
    for (;;) {
    }
}
