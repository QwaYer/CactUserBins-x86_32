extern int cact_ub_readlink(char **argv, int argc);
int main(int argc, char *argv[]) {
    return cact_ub_readlink((char **)argv, argc);
}
