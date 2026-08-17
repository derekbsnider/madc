int sink;

static int bump(int *p) {
	*p += 1;
	return *p;
}

int main(void) {
	int x = 0;
	asm volatile ("" : : "r" (&x) : "memory");
	asm volatile ("" : : "r" (bump(&x)) : "memory");
	if (x != 1)
		return 1;
	sink = x;
	return 0;
}
