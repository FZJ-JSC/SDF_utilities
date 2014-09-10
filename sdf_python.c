#include <Python.h>
#include <numpy/arrayobject.h>
#include <structmember.h>
#include "sdf.h"

#if PY_MAJOR_VERSION < 3
    #define PyInt_FromLong PyLong_FromLong
#endif

static const int typemap[] = {
    0,
    PyArray_UINT32,
    PyArray_UINT64,
    PyArray_FLOAT,
    PyArray_DOUBLE,
#ifdef NPY_FLOAT128
    PyArray_FLOAT128,
#else
    0,
#endif
    PyArray_CHAR,
    PyArray_CHAR,
};


typedef struct {
    PyObject_HEAD
    sdf_file_t *h;
} SDFObject;


static PyObject *
SDF_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    sdf_file_t *h;
    const char *file;
    SDFObject *self;
    int convert = 0, use_mmap = 1, mode = SDF_READ;
    comm_t comm = 0;

    if (!PyArg_ParseTuple(args, "s|i", &file, &convert))
        return NULL;

    self = (SDFObject*)type->tp_alloc(type, 0);
    if (self == NULL)
        return NULL;

    h = sdf_open(file, comm, mode, use_mmap);
    self->h = h;
    if (!self->h) {
        Py_DECREF(self);
        return NULL;
    }

    if (convert) h->use_float = 1;

    return (PyObject*)self;
}


static void
SDF_dealloc(PyObject* self)
{
    SDFObject *pyo = (SDFObject*)self;
    if (pyo->h) sdf_close(pyo->h);
    self->ob_type->tp_free(self);
}


static void setup_mesh(sdf_file_t *h, PyObject *dict)
{
    sdf_block_t *b = h->current_block;
    int i, n, ndims;
    size_t l1, l2;
    char *label;
    void *grid;
    PyObject *sub;
    npy_intp dims[3] = {0,0,0};

    sdf_read_data(h);

    for (n = 0; n < b->ndims; n++) {
        ndims = (int)b->dims[n];

        l1 = strlen(b->name);
        l2 = strlen(b->dim_labels[n]);
        label = malloc(l1 + l2 + 2);
        memcpy(label, b->name, l1);
        label[l1] = '/';
        memcpy(label+l1+1, b->dim_labels[n], l2+1);

        l1 = strlen(b->id);
        grid = b->grids[n];
        /* Hack to node-centre the cartesian grid */
        if (strncmp(b->id, "grid", l1+1) == 0) {
            ndims--;
            if (b->datatype_out == SDF_DATATYPE_REAL4) {
                float v1, v2, *ptr, *out;
                ptr = grid;
                grid = out = malloc(ndims * sizeof(*out));
                for (i = 0; i < ndims; i++) {
                    v1 = *ptr;
                    v2 = *(ptr+1);
                    *out++ = 0.5 * (v1 + v2);
                    ptr++;
                }
            } else {
                double v1, v2, *ptr, *out;
                ptr = grid;
                grid = out = malloc(ndims * sizeof(*out));
                for (i = 0; i < ndims; i++) {
                    v1 = *ptr;
                    v2 = *(ptr+1);
                    *out++ = 0.5 * (v1 + v2);
                    ptr++;
                }
            }

            dims[0] = ndims;
            sub = PyArray_NewFromDescr(&PyArray_Type,
                PyArray_DescrFromType(typemap[b->datatype_out]), 1,
                dims, NULL, grid, NPY_F_CONTIGUOUS, NULL);
            PyDict_SetItemString(dict, label, sub);
            Py_DECREF(sub);

            /* Now add the original grid with "_node" appended */
            ndims++;
            grid = b->grids[n];

            l1 = strlen(b->name);
            l2 = strlen(b->dim_labels[n]);
            label = malloc(l1 + l2 + 2 + 5);
            memcpy(label, b->name, l1);
            memcpy(label+l1, "_node", 5);
            label[l1+5] = '/';
            memcpy(label+l1+6, b->dim_labels[n], l2+1);
        }

        dims[0] = ndims;
        sub = PyArray_NewFromDescr(&PyArray_Type,
            PyArray_DescrFromType(typemap[b->datatype_out]), 1,
            dims, NULL, grid, NPY_F_CONTIGUOUS, NULL);
        PyDict_SetItemString(dict, label, sub);
        Py_DECREF(sub);
    }
}


#define SET_ENTRY(type,value) do { \
        PyObject *sub; \
        sub = Py_BuildValue(#type, h->value); \
        PyDict_SetItemString(dict, #value, sub); \
        Py_DECREF(sub); \
    } while (0)

#define SET_BOOL(value) \
    if (h->value) PyDict_SetItemString(dict, #value, Py_True); \
    else PyDict_SetItemString(dict, #value, Py_False)

static PyObject *fill_header(sdf_file_t *h)
{
    PyObject *dict;

    dict = PyDict_New();

    SET_ENTRY(i, file_version);
    SET_ENTRY(i, file_revision);
    SET_ENTRY(s, code_name);
    SET_ENTRY(i, step);
    SET_ENTRY(d, time);
    SET_ENTRY(i, jobid1);
    SET_ENTRY(i, jobid2);
    SET_ENTRY(i, code_io_version);
    SET_BOOL(restart_flag);
    SET_BOOL(other_domains);

    return dict;
}

static PyObject* SDF_read(SDFObject *self, PyObject *args)
{
    sdf_file_t *h;
    sdf_block_t *b;
    PyObject *dict, *sub;
    int i, n;
    double dd;
    long il;
    long long ll;
    npy_intp dims[3] = {0,0,0};

    h = self->h;
    sdf_read_blocklist(h);
    dict = PyDict_New();

    // Add header
    sub = fill_header(h);
    PyDict_SetItemString(dict, "Header", sub);
    Py_DECREF(sub);

    b = h->current_block = h->blocklist;
    for (i = 0; i < h->nblocks; i++) {
        switch(b->blocktype) {
          case SDF_BLOCKTYPE_PLAIN_MESH:
          case SDF_BLOCKTYPE_POINT_MESH:
            setup_mesh(h, dict);
            break;
          case SDF_BLOCKTYPE_PLAIN_VARIABLE:
          case SDF_BLOCKTYPE_POINT_VARIABLE:
            for (n = 0; n < b->ndims; n++) dims[n] = (int)b->dims[n];
            sdf_read_data(h);
            sub = PyArray_NewFromDescr(&PyArray_Type,
                PyArray_DescrFromType(typemap[b->datatype_out]), b->ndims,
                dims, NULL, b->data, NPY_F_CONTIGUOUS, NULL);
            PyDict_SetItemString(dict, b->name, sub);
            Py_DECREF(sub);
            break;
          case SDF_BLOCKTYPE_CONSTANT:
            switch(b->datatype) {
              case SDF_DATATYPE_REAL4:
                dd = *((float*)b->const_value);
                sub = PyFloat_FromDouble(dd);
                PyDict_SetItemString(dict, b->name, sub);
                Py_DECREF(sub);
                break;
              case SDF_DATATYPE_REAL8:
                dd = *((double*)b->const_value);
                sub = PyFloat_FromDouble(dd);
                PyDict_SetItemString(dict, b->name, sub);
                Py_DECREF(sub);
                break;
              case SDF_DATATYPE_INTEGER4:
                il = *((int32_t*)b->const_value);
                sub = PyLong_FromLong(il);
                PyDict_SetItemString(dict, b->name, sub);
                Py_DECREF(sub);
                break;
              case SDF_DATATYPE_INTEGER8:
                ll = *((int64_t*)b->const_value);
                sub = PyLong_FromLongLong(ll);
                PyDict_SetItemString(dict, b->name, sub);
                Py_DECREF(sub);
                break;
            }
            break;
        }
        b = h->current_block = b->next;
    }

    return dict;
}


static PyMethodDef SDF_methods[] = {
    {"read", (PyCFunction)SDF_read, METH_VARARGS,
        "Reads the SDF data and returns a dictionary of NumPy arrays" },
    {NULL}
};


static PyTypeObject SDF_type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "sdf.SDF",                 /* tp_name           */
    sizeof(SDFObject),         /* tp_basicsize      */
    0,                         /* tp_itemsize       */
    SDF_dealloc,               /* tp_dealloc */
    0,                         /* tp_print          */
    0,                         /* tp_getattr        */
    0,                         /* tp_setattr        */
    0,                         /* tp_compare        */
    0,                         /* tp_repr           */
    0,                         /* tp_as_number      */
    0,                         /* tp_as_sequence    */
    0,                         /* tp_as_mapping     */
    0,                         /* tp_hash           */
    0,                         /* tp_call           */
    0,                         /* tp_str            */
    0,                         /* tp_getattro       */
    0,                         /* tp_setattro       */
    0,                         /* tp_as_buffer      */
    Py_TPFLAGS_DEFAULT,        /* tp_flags          */
    "SDF constructor accepts two arguments.\n"
    "The first is the SDF filename to open. This argument is mandatory.\n"
    "The second argument is an optional integer. If it is non-zero then the\n"
    "data is converted from double precision to single.",  /* tp_doc      */
    0,                         /* tp_traverse       */
    0,                         /* tp_clear          */
    0,                         /* tp_richcompare    */
    0,                         /* tp_weaklistoffset */
    0,                         /* tp_iter           */
    0,                         /* tp_iternext       */
    SDF_methods,               /* tp_methods        */
    0,                         /* tp_members        */
    0,                         /* tp_getset         */
    0,                         /* tp_base           */
    0,                         /* tp_dict           */
    0,                         /* tp_descr_get      */
    0,                         /* tp_descr_set      */
    0,                         /* tp_dictoffset     */
    0,                         /* tp_init           */
    0,                         /* tp_alloc          */
    SDF_new,                   /* tp_new            */
};


#if PY_MAJOR_VERSION >= 3
  #define MOD_ERROR_VAL NULL
  #define MOD_SUCCESS_VAL(val) val
  #define MOD_INIT(name) PyMODINIT_FUNC PyInit_##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          static struct PyModuleDef moduledef = { \
            PyModuleDef_HEAD_INIT, name, doc, -1, methods, }; \
          ob = PyModule_Create(&moduledef);
#else
  #define MOD_ERROR_VAL
  #define MOD_SUCCESS_VAL(val)
  #define MOD_INIT(name) void init##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          ob = Py_InitModule3(name, methods, doc);
#endif


MOD_INIT(sdf)
{
    PyObject *m;

    MOD_DEF(m, "sdf", "SDF file reading library", NULL)

    if (m == NULL)
        return MOD_ERROR_VAL;

    if (PyType_Ready(&SDF_type) < 0)
        return MOD_ERROR_VAL;

    Py_INCREF(&SDF_type);
    if (PyModule_AddObject(m, "SDF", (PyObject *) &SDF_type) < 0)
        return MOD_ERROR_VAL;

    import_array();   /* required NumPy initialization */

    return MOD_SUCCESS_VAL(m);
}
