int never(void) {
	return 0;
}

int main(void) {
	if (never())
		__builtin_abort();
	return 0;
}
