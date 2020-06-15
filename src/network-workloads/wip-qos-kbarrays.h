
static size_t array_alloc_size = 1001;

/* 'Dynamic' array for storing msg/req times */
typedef struct 
{
  float *record;
  size_t used;
  size_t size;
} Array;
static void initArray(Array *a, size_t array_size) 
{
    a->record = (float *)malloc(array_size * sizeof(float));
    if (!a->record && array_size != 0){
        printf("ERROR: Unable to initialize records array.\n");
        exit(0);
    }
    a->used = 0;
    a->size = array_size;
}
static void insertArray(Array *a, size_t index, float element) 
{
    if (index < a->size){
        a->record[index] = element;
	a->used++;
        return;
    }
    else {
        a->record = (float *)realloc(a->record, (a->size+array_alloc_size) * sizeof(float));
        if (!a->record){
            printf("ERROR: Unable to grow records array from %d to %d bytes.\n", a->size, a->size+array_alloc_size);
            exit(0);
        }
        a->size += array_alloc_size;
    }
    a->record[index] = element;
    a->used++;
}
static void freeArray(Array *a) 
{
  free(a->record);
  a->record = NULL;
  a->used = 0;
  a->size = 0;
}
