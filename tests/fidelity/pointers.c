int deref(int *p)
{
	int x;
	x = *p;
	return x;
}

int addr(void)
{
	int v;
	int *p;
	v = 5;
	p = &v;
	return *p;
}
