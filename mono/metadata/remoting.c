/*
 * remoting.c: Remoting support
 * 
 * Copyright 2002-2003 Ximian, Inc (http://www.ximian.com)
 * Copyright 2004-2009 Novell, Inc (http://www.novell.com)
 * Copyright 2011-2014 Xamarin, Inc (http://www.xamarin.com)
 *
 */

#include "config.h"

#include "mono/metadata/remoting.h"
#include "mono/metadata/marshal.h"
#include "mono/metadata/abi-details.h"
#include "mono/metadata/cominterop.h"
#include "mono/metadata/tabledefs.h"
#include "mono/metadata/exception.h"
#include "mono/metadata/debug-helpers.h"

typedef enum {
	MONO_MARSHAL_NONE,			/* No marshalling needed */
	MONO_MARSHAL_COPY,			/* Can be copied by value to the new domain */
	MONO_MARSHAL_COPY_OUT,		/* out parameter that needs to be copied back to the original instance */
	MONO_MARSHAL_SERIALIZE		/* Value needs to be serialized into the new domain */
} MonoXDomainMarshalType;

#ifndef DISABLE_REMOTING

#define OPDEF(a,b,c,d,e,f,g,h,i,j) \
	a = i,

enum {
#include "mono/cil/opcode.def"
	LAST = 0xff
};
#undef OPDEF

struct _MonoRemotingMethods {
	MonoMethod *invoke;
	MonoMethod *invoke_with_check;
	MonoMethod *xdomain_invoke;
	MonoMethod *xdomain_dispatch;
};

typedef struct _MonoRemotingMethods MonoRemotingMethods;

static MonoObject *
mono_remoting_wrapper (MonoMethod *method, gpointer *params);

static gint32
mono_marshal_set_domain_by_id (gint32 id, MonoBoolean push);

static gboolean
mono_marshal_check_domain_image (gint32 domain_id, MonoImage *image);

MONO_API void
mono_upgrade_remote_class_wrapper (MonoReflectionType *rtype, MonoTransparentProxy *tproxy);

static MonoXDomainMarshalType
mono_get_xdomain_marshal_type (MonoType *t);

static void
mono_marshal_xdomain_copy_out_value (MonoObject *src, MonoObject *dst);

static MonoReflectionType *
type_from_handle (MonoType *handle);

static mono_mutex_t remoting_mutex;
static gboolean remoting_mutex_inited;

static MonoClass *byte_array_class;
#ifndef DISABLE_JIT
static MonoMethod *method_rs_serialize, *method_rs_deserialize, *method_exc_fixexc, *method_rs_appdomain_target;
static MonoMethod *method_set_call_context, *method_needs_context_sink, *method_rs_serialize_exc;
#endif

static void
register_icall (gpointer func, const char *name, const char *sigstr, gboolean save)
{
	MonoMethodSignature *sig = mono_create_icall_signature (sigstr);

	mono_register_jit_icall (func, name, sig, save);
}

static inline void
remoting_lock (void)
{
	g_assert (remoting_mutex_inited);
	mono_mutex_lock (&remoting_mutex);
}

static inline void
remoting_unlock (void)
{
	g_assert (remoting_mutex_inited);
	mono_mutex_unlock (&remoting_mutex);
}

/*
 * Return the hash table pointed to by VAR, lazily creating it if neccesary.
 */
static GHashTable*
get_cache (GHashTable **var, GHashFunc hash_func, GCompareFunc equal_func)
{
	if (!(*var)) {
		remoting_lock ();
		if (!(*var)) {
			GHashTable *cache = 
				g_hash_table_new (hash_func, equal_func);
			mono_memory_barrier ();
			*var = cache;
		}
		remoting_unlock ();
	}
	return *var;
}

static GHashTable*
get_cache_full (GHashTable **var, GHashFunc hash_func, GCompareFunc equal_func, GDestroyNotify key_destroy_func, GDestroyNotify value_destroy_func)
{
	if (!(*var)) {
		remoting_lock ();
		if (!(*var)) {
			GHashTable *cache = 
				g_hash_table_new_full (hash_func, equal_func, key_destroy_func, value_destroy_func);
			mono_memory_barrier ();
			*var = cache;
		}
		remoting_unlock ();
	}
	return *var;
}

void
mono_remoting_init (void)
{
	mono_mutex_init (&remoting_mutex);
	remoting_mutex_inited = TRUE;
}

static void
mono_remoting_marshal_init (void)
{
	MonoClass *klass;

	static gboolean module_initialized = FALSE;
	static gboolean icalls_registered = FALSE;

	if (module_initialized)
		return;

	byte_array_class = mono_array_class_get (mono_defaults.byte_class, 1);

#ifndef DISABLE_JIT
	klass = mono_class_from_name (mono_defaults.corlib, "System.Runtime.Remoting", "RemotingServices");
	method_rs_serialize = mono_class_get_method_from_name (klass, "SerializeCallData", -1);
	g_assert (method_rs_serialize);
	method_rs_deserialize = mono_class_get_method_from_name (klass, "DeserializeCallData", -1);
	g_assert (method_rs_deserialize);
	method_rs_serialize_exc = mono_class_get_method_from_name (klass, "SerializeExceptionData", -1);
	g_assert (method_rs_serialize_exc);
	
	klass = mono_defaults.real_proxy_class;
	method_rs_appdomain_target = mono_class_get_method_from_name (klass, "GetAppDomainTarget", -1);
	g_assert (method_rs_appdomain_target);
	
	klass = mono_defaults.exception_class;
	method_exc_fixexc = mono_class_get_method_from_name (klass, "FixRemotingException", -1);
	g_assert (method_exc_fixexc);

	klass = mono_class_from_name (mono_defaults.corlib, "System.Runtime.Remoting.Messaging", "CallContext");
	method_set_call_context = mono_class_get_method_from_name (klass, "SetCurrentCallContext", -1);
	g_assert (method_set_call_context);

	klass = mono_class_from_name (mono_defaults.corlib, "System.Runtime.Remoting.Contexts", "Context");
	method_needs_context_sink = mono_class_get_method_from_name (klass, "get_NeedsContextSink", -1);
	g_assert (method_needs_context_sink);
#endif	

	mono_loader_lock ();

	if (!icalls_registered) {
		register_icall (type_from_handle, "type_from_handle", "object ptr", FALSE);
		register_icall (mono_marshal_set_domain_by_id, "mono_marshal_set_domain_by_id", "int32 int32 int32", FALSE);
		register_icall (mono_marshal_check_domain_image, "mono_marshal_check_domain_image", "int32 int32 ptr", FALSE);
		register_icall (mono_marshal_xdomain_copy_value, "mono_marshal_xdomain_copy_value", "object object", FALSE);
		register_icall (mono_marshal_xdomain_copy_out_value, "mono_marshal_xdomain_copy_out_value", "void object object", FALSE);
		register_icall (mono_remoting_wrapper, "mono_remoting_wrapper", "object ptr ptr", FALSE);
		register_icall (mono_upgrade_remote_class_wrapper, "mono_upgrade_remote_class_wrapper", "void object object", FALSE);
	}

	icalls_registered = TRUE;

	mono_loader_unlock ();

	module_initialized = TRUE;
}

static MonoReflectionType *
type_from_handle (MonoType *handle)
{
	MonoDomain *domain = mono_domain_get (); 
	MonoClass *klass = mono_class_from_mono_type (handle);

	mono_class_init (klass);
	return mono_type_get_object (domain, handle);
}

#ifndef DISABLE_JIT
static int
mono_mb_emit_proxy_check (MonoMethodBuilder *mb, int branch_code)
{
	int pos;
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoObject, vtable));
	mono_mb_emit_byte (mb, CEE_LDIND_I);
	mono_mb_emit_icon (mb, MONO_STRUCT_OFFSET (MonoVTable, klass));
	mono_mb_emit_byte (mb, CEE_ADD);
	mono_mb_emit_byte (mb, CEE_LDIND_I);
	mono_mb_emit_byte (mb, MONO_CUSTOM_PREFIX);
	mono_mb_emit_byte (mb, CEE_MONO_CLASSCONST);
	mono_mb_emit_i4 (mb, mono_mb_add_data (mb, mono_defaults.transparent_proxy_class));
	pos = mono_mb_emit_branch (mb, branch_code);
	return pos;
}

static int
mono_mb_emit_xdomain_check (MonoMethodBuilder *mb, int branch_code)
{
	int pos;
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoTransparentProxy, rp));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoRealProxy, target_domain_id));
	mono_mb_emit_byte (mb, CEE_LDIND_I4);
	mono_mb_emit_icon (mb, -1);
	pos = mono_mb_emit_branch (mb, branch_code);
	return pos;
}

static int
mono_mb_emit_contextbound_check (MonoMethodBuilder *mb, int branch_code)
{
	static int offset = -1;
	static guint8 mask;

	if (offset < 0)
		mono_marshal_find_bitfield_offset (MonoClass, contextbound, &offset, &mask);

	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoTransparentProxy, remote_class));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoRemoteClass, proxy_class));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_ldflda (mb, offset);
	mono_mb_emit_byte (mb, CEE_LDIND_U1);
	mono_mb_emit_icon (mb, mask);
	mono_mb_emit_byte (mb, CEE_AND);
	mono_mb_emit_icon (mb, 0);
	return mono_mb_emit_branch (mb, branch_code);
}
#endif /* !DISABLE_JIT */

static inline MonoMethod*
mono_marshal_remoting_find_in_cache (MonoMethod *method, int wrapper_type)
{
	MonoMethod *res = NULL;
	MonoRemotingMethods *wrps;

	mono_marshal_lock_internal ();
	if (method->is_inflated && ((MonoMethodInflated *)method)->owner->wrapper_caches.remoting_invoke_cache) {
		MonoMethodInflated *imethod = (MonoMethodInflated *)method;
		wrps = g_hash_table_lookup (imethod->owner->wrapper_caches.remoting_invoke_cache, method);
	} else if (method->klass->image->wrapper_caches.remoting_invoke_cache)
		wrps = g_hash_table_lookup (method->klass->image->wrapper_caches.remoting_invoke_cache, method);
	else
		wrps = NULL;

	if (wrps) {
		switch (wrapper_type) {
		case MONO_WRAPPER_REMOTING_INVOKE: res = wrps->invoke; break;
		case MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK: res = wrps->invoke_with_check; break;
		case MONO_WRAPPER_XDOMAIN_INVOKE: res = wrps->xdomain_invoke; break;
		case MONO_WRAPPER_XDOMAIN_DISPATCH: res = wrps->xdomain_dispatch; break;
		}
	}
	
	/* it is important to do the unlock after the load from wrps, since in
	 * mono_remoting_mb_create_and_cache () we drop the marshal lock to be able
	 * to take the loader lock and some other thread may set the fields.
	 */
	mono_marshal_unlock_internal ();
	return res;
}

/* Create the method from the builder and place it in the cache */
static inline MonoMethod*
mono_remoting_mb_create_and_cache (MonoMethod *key, MonoMethodBuilder *mb, 
								MonoMethodSignature *sig, int max_stack)
{
	MonoMethod **res = NULL;
	MonoRemotingMethods *wrps;
	GHashTable *cache;

	if (key->is_inflated) {
		MonoMethodInflated *imethod = (MonoMethodInflated *)key;
		cache = get_cache_full (&imethod->owner->wrapper_caches.remoting_invoke_cache, mono_aligned_addr_hash, NULL, NULL, g_free);
	} else
		cache = get_cache_full (&key->klass->image->wrapper_caches.remoting_invoke_cache, mono_aligned_addr_hash, NULL, NULL, g_free);

	mono_marshal_lock_internal ();
	wrps = g_hash_table_lookup (cache, key);
	if (!wrps) {
		wrps = g_new0 (MonoRemotingMethods, 1);
		g_hash_table_insert (cache, key, wrps);
	}

	switch (mb->method->wrapper_type) {
	case MONO_WRAPPER_REMOTING_INVOKE: res = &wrps->invoke; break;
	case MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK: res = &wrps->invoke_with_check; break;
	case MONO_WRAPPER_XDOMAIN_INVOKE: res = &wrps->xdomain_invoke; break;
	case MONO_WRAPPER_XDOMAIN_DISPATCH: res = &wrps->xdomain_dispatch; break;
	default: g_assert_not_reached (); break;
	}
	mono_marshal_unlock_internal ();

	if (*res == NULL) {
		MonoMethod *newm;
		newm = mono_mb_create_method (mb, sig, max_stack);

		mono_marshal_lock_internal ();
		if (!*res) {
			*res = newm;
			mono_marshal_set_wrapper_info (*res, key);
			mono_marshal_unlock_internal ();
		} else {
			mono_marshal_unlock_internal ();
			mono_free_method (newm);
		}
	}

	return *res;
}		

static MonoObject *
mono_remoting_wrapper (MonoMethod *method, gpointer *params)
{
	MonoMethodMessage *msg;
	MonoTransparentProxy *this_obj;
	MonoObject *res, *exc;
	MonoArray *out_args;

	this_obj = *((MonoTransparentProxy **)params [0]);

	g_assert (this_obj);
	g_assert (((MonoObject *)this_obj)->vtable->klass == mono_defaults.transparent_proxy_class);
	
	/* skip the this pointer */
	params++;

	if (mono_class_is_contextbound (this_obj->remote_class->proxy_class) && this_obj->rp->context == (MonoObject *) mono_context_get ())
	{
		int i;
		MonoMethodSignature *sig = mono_method_signature (method);
		int count = sig->param_count;
		gpointer* mparams = (gpointer*) alloca(count*sizeof(gpointer));

		for (i=0; i<count; i++) {
			MonoClass *class = mono_class_from_mono_type (sig->params [i]);
			if (class->valuetype) {
				if (sig->params [i]->byref) {
					mparams[i] = *((gpointer *)params [i]);
				} else {
					/* runtime_invoke expects a boxed instance */
					if (mono_class_is_nullable (mono_class_from_mono_type (sig->params [i])))
						mparams[i] = mono_nullable_box (params [i], class);
					else
						mparams[i] = params [i];
				}
			} else {
				mparams[i] = *((gpointer**)params [i]);
			}
		}

		return mono_runtime_invoke (method, method->klass->valuetype? mono_object_unbox ((MonoObject*)this_obj): this_obj, mparams, NULL);
	}

	msg = mono_method_call_message_new (method, params, NULL, NULL, NULL);

	res = mono_remoting_invoke ((MonoObject *)this_obj->rp, msg, &exc, &out_args);

	if (exc)
		mono_raise_exception ((MonoException *)exc);

	mono_method_return_message_restore (method, params, out_args);

	return res;
} 

MonoMethod *
mono_marshal_get_remoting_invoke (MonoMethod *method)
{
	MonoMethodSignature *sig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	int params_var;

	g_assert (method);

	if (method->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE || method->wrapper_type == MONO_WRAPPER_XDOMAIN_INVOKE)
		return method;

	/* this seems to be the best plase to put this, as all remoting invokes seem to get filtered through here */
#ifndef DISABLE_COM
	if (mono_class_is_com_object (method->klass) || method->klass == mono_class_get_com_object_class ()) {
		MonoVTable *vtable = mono_class_vtable (mono_domain_get (), method->klass);
		g_assert (vtable); /*FIXME do proper error handling*/

		if (!mono_vtable_is_remote (vtable)) {
			return mono_cominterop_get_invoke (method);
		}
	}
#endif

	sig = mono_signature_no_pinvoke (method);

	/* we cant remote methods without this pointer */
	if (!sig->hasthis)
		return method;

	if ((res = mono_marshal_remoting_find_in_cache (method, MONO_WRAPPER_REMOTING_INVOKE)))
		return res;

	mono_remoting_marshal_init ();

	mb = mono_mb_new (method->klass, method->name, MONO_WRAPPER_REMOTING_INVOKE);

#ifndef DISABLE_JIT
	mb->method->save_lmf = 1;

	params_var = mono_mb_emit_save_args (mb, sig, TRUE);

	mono_mb_emit_ptr (mb, method);
	mono_mb_emit_ldloc (mb, params_var);
	mono_mb_emit_icall (mb, mono_remoting_wrapper);
	mono_marshal_emit_thread_interrupt_checkpoint (mb);

	if (sig->ret->type == MONO_TYPE_VOID) {
		mono_mb_emit_byte (mb, CEE_POP);
		mono_mb_emit_byte (mb, CEE_RET);
	} else {
		 mono_mb_emit_restore_result (mb, sig->ret);
	}
#endif

	res = mono_remoting_mb_create_and_cache (method, mb, sig, sig->param_count + 16);
	mono_mb_free (mb);

	return res;
}

/* mono_marshal_xdomain_copy_out_value()
 * Copies the contents of the src instance into the dst instance. src and dst
 * must have the same type, and if they are arrays, the same size.
 */
static void
mono_marshal_xdomain_copy_out_value (MonoObject *src, MonoObject *dst)
{
	if (src == NULL || dst == NULL) return;
	
	g_assert (mono_object_class (src) == mono_object_class (dst));

	switch (mono_object_class (src)->byval_arg.type) {
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY: {
		int mt = mono_get_xdomain_marshal_type (&(mono_object_class (src)->element_class->byval_arg));
		if (mt == MONO_MARSHAL_SERIALIZE) return;
		if (mt == MONO_MARSHAL_COPY) {
			int i, len = mono_array_length ((MonoArray *)dst);
			for (i = 0; i < len; i++) {
				MonoObject *item = mono_array_get ((MonoArray *)src, gpointer, i);
				mono_array_setref ((MonoArray *)dst, i, mono_marshal_xdomain_copy_value (item));
			}
		} else {
			mono_array_full_copy ((MonoArray *)src, (MonoArray *)dst);
		}
		return;
	}
	default:
		break;
	}

}


#if !defined (DISABLE_JIT)
static void
mono_marshal_emit_xdomain_copy_value (MonoMethodBuilder *mb, MonoClass *pclass)
{
	mono_mb_emit_icall (mb, mono_marshal_xdomain_copy_value);
	mono_mb_emit_op (mb, CEE_CASTCLASS, pclass);
}

static void
mono_marshal_emit_xdomain_copy_out_value (MonoMethodBuilder *mb, MonoClass *pclass)
{
	mono_mb_emit_icall (mb, mono_marshal_xdomain_copy_out_value);
}
#endif

/* mono_marshal_supports_fast_xdomain()
 * Returns TRUE if the method can use the fast xdomain wrapper.
 */
static gboolean
mono_marshal_supports_fast_xdomain (MonoMethod *method)
{
	return !mono_class_is_contextbound (method->klass) &&
		   !((method->flags & METHOD_ATTRIBUTE_SPECIAL_NAME) && (strcmp (".ctor", method->name) == 0));
}

static gint32
mono_marshal_set_domain_by_id (gint32 id, MonoBoolean push)
{
	MonoDomain *current_domain = mono_domain_get ();
	MonoDomain *domain = mono_domain_get_by_id (id);

	if (!domain || !mono_domain_set (domain, FALSE))	
		mono_raise_exception (mono_get_exception_appdomain_unloaded ());

	if (push)
		mono_thread_push_appdomain_ref (domain);
	else
		mono_thread_pop_appdomain_ref ();

	return current_domain->domain_id;
}

#if !defined (DISABLE_JIT)
static void
mono_marshal_emit_switch_domain (MonoMethodBuilder *mb)
{
	mono_mb_emit_icall (mb, mono_marshal_set_domain_by_id);
}

/* mono_marshal_emit_load_domain_method ()
 * Loads into the stack a pointer to the code of the provided method for
 * the current domain.
 */
static void
mono_marshal_emit_load_domain_method (MonoMethodBuilder *mb, MonoMethod *method)
{
	/* We need a pointer to the method for the running domain (not the domain
	 * that compiles the method).
	 */
	mono_mb_emit_ptr (mb, method);
	mono_mb_emit_icall (mb, mono_compile_method);
}
#endif

/* mono_marshal_check_domain_image ()
 * Returns TRUE if the image is loaded in the specified
 * application domain.
 */
static gboolean
mono_marshal_check_domain_image (gint32 domain_id, MonoImage *image)
{
	MonoAssembly* ass;
	GSList *tmp;
	
	MonoDomain *domain = mono_domain_get_by_id (domain_id);
	if (!domain)
		return FALSE;
	
	mono_domain_assemblies_lock (domain);
	for (tmp = domain->domain_assemblies; tmp; tmp = tmp->next) {
		ass = tmp->data;
		if (ass->image == image)
			break;
	}
	mono_domain_assemblies_unlock (domain);
	
	return tmp != NULL;
}

/* mono_marshal_get_xappdomain_dispatch ()
 * Generates a method that dispatches a method call from another domain into
 * the current domain.
 */
static MonoMethod *
mono_marshal_get_xappdomain_dispatch (MonoMethod *method, int *marshal_types, int complex_count, int complex_out_count, int ret_marshal_type)
{
	MonoMethodSignature *sig, *csig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	int i, j, param_index, copy_locals_base;
	MonoClass *ret_class = NULL;
	int loc_array=0, loc_return=0, loc_serialized_exc=0;
	MonoExceptionClause *main_clause;
	int pos, pos_leave;
	gboolean copy_return;

	if ((res = mono_marshal_remoting_find_in_cache (method, MONO_WRAPPER_XDOMAIN_DISPATCH)))
		return res;

	sig = mono_method_signature (method);
	copy_return = (sig->ret->type != MONO_TYPE_VOID && ret_marshal_type != MONO_MARSHAL_SERIALIZE);

	j = 0;
	csig = mono_metadata_signature_alloc (mono_defaults.corlib, 3 + sig->param_count - complex_count);
	csig->params [j++] = &mono_defaults.object_class->byval_arg;
	csig->params [j++] = &byte_array_class->this_arg;
	csig->params [j++] = &byte_array_class->this_arg;
	for (i = 0; i < sig->param_count; i++) {
		if (marshal_types [i] != MONO_MARSHAL_SERIALIZE)
			csig->params [j++] = sig->params [i];
	}
	if (copy_return)
		csig->ret = sig->ret;
	else
		csig->ret = &mono_defaults.void_class->byval_arg;
	csig->pinvoke = 1;
	csig->hasthis = FALSE;

	mb = mono_mb_new (method->klass, method->name, MONO_WRAPPER_XDOMAIN_DISPATCH);
	mb->method->save_lmf = 1;

#ifndef DISABLE_JIT
	/* Locals */

	loc_serialized_exc = mono_mb_add_local (mb, &byte_array_class->byval_arg);
	if (complex_count > 0)
		loc_array = mono_mb_add_local (mb, &mono_defaults.object_class->byval_arg);
	if (sig->ret->type != MONO_TYPE_VOID) {
		loc_return = mono_mb_add_local (mb, sig->ret);
		ret_class = mono_class_from_mono_type (sig->ret);
	}

	/* try */

	main_clause = mono_image_alloc0 (method->klass->image, sizeof (MonoExceptionClause));
	main_clause->try_offset = mono_mb_get_label (mb);

	/* Clean the call context */

	mono_mb_emit_byte (mb, CEE_LDNULL);
	mono_mb_emit_managed_call (mb, method_set_call_context, NULL);
	mono_mb_emit_byte (mb, CEE_POP);

	/* Deserialize call data */

	mono_mb_emit_ldarg (mb, 1);
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_byte (mb, CEE_DUP);
	pos = mono_mb_emit_short_branch (mb, CEE_BRFALSE_S);
	
	mono_marshal_emit_xdomain_copy_value (mb, byte_array_class);
	mono_mb_emit_managed_call (mb, method_rs_deserialize, NULL);
	
	if (complex_count > 0)
		mono_mb_emit_stloc (mb, loc_array);
	else
		mono_mb_emit_byte (mb, CEE_POP);

	mono_mb_patch_short_branch (mb, pos);

	/* Get the target object */
	
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_managed_call (mb, method_rs_appdomain_target, NULL);

	/* Load the arguments */
	
	copy_locals_base = mb->locals;
	param_index = 3;	// Index of the first non-serialized parameter of this wrapper
	j = 0;
	for (i = 0; i < sig->param_count; i++) {
		MonoType *pt = sig->params [i];
		MonoClass *pclass = mono_class_from_mono_type (pt);
		switch (marshal_types [i]) {
		case MONO_MARSHAL_SERIALIZE: {
			/* take the value from the serialized array */
			mono_mb_emit_ldloc (mb, loc_array);
			mono_mb_emit_icon (mb, j++);
			if (pt->byref) {
				if (pclass->valuetype) {
					mono_mb_emit_byte (mb, CEE_LDELEM_REF);
					mono_mb_emit_op (mb, CEE_UNBOX, pclass);
				} else {
					mono_mb_emit_op (mb, CEE_LDELEMA, pclass);
				}
			} else {
				if (pclass->valuetype) {
					mono_mb_emit_byte (mb, CEE_LDELEM_REF);
					mono_mb_emit_op (mb, CEE_UNBOX, pclass);
					mono_mb_emit_op (mb, CEE_LDOBJ, pclass);
				} else {
					mono_mb_emit_byte (mb, CEE_LDELEM_REF);
					if (pclass != mono_defaults.object_class) {
						mono_mb_emit_op (mb, CEE_CASTCLASS, pclass);
					}
				}
			}
			break;
		}
		case MONO_MARSHAL_COPY_OUT: {
			/* Keep a local copy of the value since we need to copy it back after the call */
			int copy_local = mono_mb_add_local (mb, &(pclass->byval_arg));
			mono_mb_emit_ldarg (mb, param_index++);
			mono_marshal_emit_xdomain_copy_value (mb, pclass);
			mono_mb_emit_byte (mb, CEE_DUP);
			mono_mb_emit_stloc (mb, copy_local);
			break;
		}
		case MONO_MARSHAL_COPY: {
			mono_mb_emit_ldarg (mb, param_index);
			if (pt->byref) {
				mono_mb_emit_byte (mb, CEE_DUP);
				mono_mb_emit_byte (mb, CEE_DUP);
				mono_mb_emit_byte (mb, CEE_LDIND_REF);
				mono_marshal_emit_xdomain_copy_value (mb, pclass);
				mono_mb_emit_byte (mb, CEE_STIND_REF);
			} else {
				mono_marshal_emit_xdomain_copy_value (mb, pclass);
			}
			param_index++;
			break;
		}
		case MONO_MARSHAL_NONE:
			mono_mb_emit_ldarg (mb, param_index++);
			break;
		}
	}

	/* Make the call to the real object */

	mono_marshal_emit_thread_force_interrupt_checkpoint (mb);
	
	mono_mb_emit_op (mb, CEE_CALLVIRT, method);

	if (sig->ret->type != MONO_TYPE_VOID)
		mono_mb_emit_stloc (mb, loc_return);

	/* copy back MONO_MARSHAL_COPY_OUT parameters */

	j = 0;
	param_index = 3;
	for (i = 0; i < sig->param_count; i++) {
		if (marshal_types [i] == MONO_MARSHAL_SERIALIZE) continue;
		if (marshal_types [i] == MONO_MARSHAL_COPY_OUT) {
			mono_mb_emit_ldloc (mb, copy_locals_base + (j++));
			mono_mb_emit_ldarg (mb, param_index);
			mono_marshal_emit_xdomain_copy_out_value (mb, mono_class_from_mono_type (sig->params [i]));
		}
		param_index++;
	}

	/* Serialize the return values */
	
	if (complex_out_count > 0) {
		/* Reset parameters in the array that don't need to be serialized back */
		j = 0;
		for (i = 0; i < sig->param_count; i++) {
			if (marshal_types[i] != MONO_MARSHAL_SERIALIZE) continue;
			if (!sig->params [i]->byref) {
				mono_mb_emit_ldloc (mb, loc_array);
				mono_mb_emit_icon (mb, j);
				mono_mb_emit_byte (mb, CEE_LDNULL);
				mono_mb_emit_byte (mb, CEE_STELEM_REF);
			}
			j++;
		}
	
		/* Add the return value to the array */
	
		if (ret_marshal_type == MONO_MARSHAL_SERIALIZE) {
			mono_mb_emit_ldloc (mb, loc_array);
			mono_mb_emit_icon (mb, complex_count);	/* The array has an additional slot to hold the ret value */
			mono_mb_emit_ldloc (mb, loc_return);

			g_assert (ret_class); /*FIXME properly fail here*/
			if (ret_class->valuetype) {
				mono_mb_emit_op (mb, CEE_BOX, ret_class);
			}
			mono_mb_emit_byte (mb, CEE_STELEM_REF);
		}
	
		/* Serialize */
	
		mono_mb_emit_ldarg (mb, 1);
		mono_mb_emit_ldloc (mb, loc_array);
		mono_mb_emit_managed_call (mb, method_rs_serialize, NULL);
		mono_mb_emit_byte (mb, CEE_STIND_REF);
	} else if (ret_marshal_type == MONO_MARSHAL_SERIALIZE) {
		mono_mb_emit_ldarg (mb, 1);
		mono_mb_emit_ldloc (mb, loc_return);
		if (ret_class->valuetype) {
			mono_mb_emit_op (mb, CEE_BOX, ret_class);
		}
		mono_mb_emit_managed_call (mb, method_rs_serialize, NULL);
		mono_mb_emit_byte (mb, CEE_STIND_REF);
	} else {
		mono_mb_emit_ldarg (mb, 1);
		mono_mb_emit_byte (mb, CEE_LDNULL);
		mono_mb_emit_managed_call (mb, method_rs_serialize, NULL);
		mono_mb_emit_byte (mb, CEE_STIND_REF);
	}

	mono_mb_emit_ldarg (mb, 2);
	mono_mb_emit_byte (mb, CEE_LDNULL);
	mono_mb_emit_byte (mb, CEE_STIND_REF);
	pos_leave = mono_mb_emit_branch (mb, CEE_LEAVE);

	/* Main exception catch */
	main_clause->flags = MONO_EXCEPTION_CLAUSE_NONE;
	main_clause->try_len = mono_mb_get_pos (mb) - main_clause->try_offset;
	main_clause->data.catch_class = mono_defaults.object_class;
	
	/* handler code */
	main_clause->handler_offset = mono_mb_get_label (mb);
	mono_mb_emit_managed_call (mb, method_rs_serialize_exc, NULL);
	mono_mb_emit_stloc (mb, loc_serialized_exc);
	mono_mb_emit_ldarg (mb, 2);
	mono_mb_emit_ldloc (mb, loc_serialized_exc);
	mono_mb_emit_byte (mb, CEE_STIND_REF);
	mono_mb_emit_branch (mb, CEE_LEAVE);
	main_clause->handler_len = mono_mb_get_pos (mb) - main_clause->handler_offset;
	/* end catch */

	mono_mb_patch_branch (mb, pos_leave);
	
	if (copy_return)
		mono_mb_emit_ldloc (mb, loc_return);

	mono_mb_emit_byte (mb, CEE_RET);

	mono_mb_set_clauses (mb, 1, main_clause);
#endif

	res = mono_remoting_mb_create_and_cache (method, mb, csig, csig->param_count + 16);
	mono_mb_free (mb);

	return res;
}

/* mono_marshal_get_xappdomain_invoke ()
 * Generates a fast remoting wrapper for cross app domain calls.
 */
MonoMethod *
mono_marshal_get_xappdomain_invoke (MonoMethod *method)
{
	MonoMethodSignature *sig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	int i, j, complex_count, complex_out_count, copy_locals_base;
	int *marshal_types;
	MonoClass *ret_class = NULL;
	MonoMethod *xdomain_method;
	int ret_marshal_type = MONO_MARSHAL_NONE;
	int loc_array=0, loc_serialized_data=-1, loc_real_proxy;
	int loc_old_domainid, loc_domainid, loc_return=0, loc_serialized_exc=0, loc_context;
	int pos, pos_dispatch, pos_noex;
	gboolean copy_return = FALSE;

	g_assert (method);
	
	if (method->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE || method->wrapper_type == MONO_WRAPPER_XDOMAIN_INVOKE)
		return method;

	/* we cant remote methods without this pointer */
	if (!mono_method_signature (method)->hasthis)
		return method;

	mono_remoting_marshal_init ();

	if (!mono_marshal_supports_fast_xdomain (method))
		return mono_marshal_get_remoting_invoke (method);
	
	if ((res = mono_marshal_remoting_find_in_cache (method, MONO_WRAPPER_XDOMAIN_INVOKE)))
		return res;
	
	sig = mono_signature_no_pinvoke (method);

	mb = mono_mb_new (method->klass, method->name, MONO_WRAPPER_XDOMAIN_INVOKE);
	mb->method->save_lmf = 1;

	/* Count the number of parameters that need to be serialized */

	marshal_types = alloca (sizeof (int) * sig->param_count);
	complex_count = complex_out_count = 0;
	for (i = 0; i < sig->param_count; i++) {
		MonoType *ptype = sig->params[i];
		int mt = mono_get_xdomain_marshal_type (ptype);
		
		/* If the [Out] attribute is applied to a parameter that can be internally copied,
		 * the copy will be made by reusing the original object instance
		 */
		if ((ptype->attrs & PARAM_ATTRIBUTE_OUT) != 0 && mt == MONO_MARSHAL_COPY && !ptype->byref)
			mt = MONO_MARSHAL_COPY_OUT;
		else if (mt == MONO_MARSHAL_SERIALIZE) {
			complex_count++;
			if (ptype->byref) complex_out_count++;
		}
		marshal_types [i] = mt;
	}

	if (sig->ret->type != MONO_TYPE_VOID) {
		ret_marshal_type = mono_get_xdomain_marshal_type (sig->ret);
		ret_class = mono_class_from_mono_type (sig->ret);
		copy_return = ret_marshal_type != MONO_MARSHAL_SERIALIZE;
	}
	
	/* Locals */

#ifndef DISABLE_JIT
	if (complex_count > 0)
		loc_array = mono_mb_add_local (mb, &mono_defaults.object_class->byval_arg);
	loc_serialized_data = mono_mb_add_local (mb, &byte_array_class->byval_arg);
	loc_real_proxy = mono_mb_add_local (mb, &mono_defaults.object_class->byval_arg);
	if (copy_return)
		loc_return = mono_mb_add_local (mb, sig->ret);
	loc_old_domainid = mono_mb_add_local (mb, &mono_defaults.int32_class->byval_arg);
	loc_domainid = mono_mb_add_local (mb, &mono_defaults.int32_class->byval_arg);
	loc_serialized_exc = mono_mb_add_local (mb, &byte_array_class->byval_arg);
	loc_context = mono_mb_add_local (mb, &mono_defaults.object_class->byval_arg);

	/* Save thread domain data */

	mono_mb_emit_icall (mb, mono_context_get);
	mono_mb_emit_byte (mb, CEE_DUP);
	mono_mb_emit_stloc (mb, loc_context);

	/* If the thread is not running in the default context, it needs to go
	 * through the whole remoting sink, since the context is going to change
	 */
	mono_mb_emit_managed_call (mb, method_needs_context_sink, NULL);
	pos = mono_mb_emit_short_branch (mb, CEE_BRTRUE_S);
	
	/* Another case in which the fast path can't be used: when the target domain
	 * has a different image for the same assembly.
	 */

	/* Get the target domain id */

	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoTransparentProxy, rp));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_byte (mb, CEE_DUP);
	mono_mb_emit_stloc (mb, loc_real_proxy);

	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoRealProxy, target_domain_id));
	mono_mb_emit_byte (mb, CEE_LDIND_I4);
	mono_mb_emit_stloc (mb, loc_domainid);

	/* Check if the target domain has the same image for the required assembly */

	mono_mb_emit_ldloc (mb, loc_domainid);
	mono_mb_emit_ptr (mb, method->klass->image);
	mono_mb_emit_icall (mb, mono_marshal_check_domain_image);
	pos_dispatch = mono_mb_emit_short_branch (mb, CEE_BRTRUE_S);

	/* Use the whole remoting sink to dispatch this message */

	mono_mb_patch_short_branch (mb, pos);

	mono_mb_emit_ldarg (mb, 0);
	for (i = 0; i < sig->param_count; i++)
		mono_mb_emit_ldarg (mb, i + 1);
	
	mono_mb_emit_managed_call (mb, mono_marshal_get_remoting_invoke (method), NULL);
	mono_mb_emit_byte (mb, CEE_RET);
	mono_mb_patch_short_branch (mb, pos_dispatch);

	/* Create the array that will hold the parameters to be serialized */

	if (complex_count > 0) {
		mono_mb_emit_icon (mb, (ret_marshal_type == MONO_MARSHAL_SERIALIZE && complex_out_count > 0) ? complex_count + 1 : complex_count);	/* +1 for the return type */
		mono_mb_emit_op (mb, CEE_NEWARR, mono_defaults.object_class);
	
		j = 0;
		for (i = 0; i < sig->param_count; i++) {
			MonoClass *pclass;
			if (marshal_types [i] != MONO_MARSHAL_SERIALIZE) continue;
			pclass = mono_class_from_mono_type (sig->params[i]);
			mono_mb_emit_byte (mb, CEE_DUP);
			mono_mb_emit_icon (mb, j);
			mono_mb_emit_ldarg (mb, i + 1);		/* 0=this */
			if (sig->params[i]->byref) {
				if (pclass->valuetype)
					mono_mb_emit_op (mb, CEE_LDOBJ, pclass);
				else
					mono_mb_emit_byte (mb, CEE_LDIND_REF);
			}
			if (pclass->valuetype)
				mono_mb_emit_op (mb, CEE_BOX, pclass);
			mono_mb_emit_byte (mb, CEE_STELEM_REF);
			j++;
		}
		mono_mb_emit_stloc (mb, loc_array);

		/* Serialize parameters */
	
		mono_mb_emit_ldloc (mb, loc_array);
		mono_mb_emit_managed_call (mb, method_rs_serialize, NULL);
		mono_mb_emit_stloc (mb, loc_serialized_data);
	} else {
		mono_mb_emit_byte (mb, CEE_LDNULL);
		mono_mb_emit_managed_call (mb, method_rs_serialize, NULL);
		mono_mb_emit_stloc (mb, loc_serialized_data);
	}

	/* switch domain */

	mono_mb_emit_ldloc (mb, loc_domainid);
	mono_mb_emit_byte (mb, CEE_LDC_I4_1);
	mono_marshal_emit_switch_domain (mb);
	mono_mb_emit_stloc (mb, loc_old_domainid);

	/* Load the arguments */
	
	mono_mb_emit_ldloc (mb, loc_real_proxy);
	mono_mb_emit_ldloc_addr (mb, loc_serialized_data);
	mono_mb_emit_ldloc_addr (mb, loc_serialized_exc);

	copy_locals_base = mb->locals;
	for (i = 0; i < sig->param_count; i++) {
		switch (marshal_types [i]) {
		case MONO_MARSHAL_SERIALIZE:
			continue;
		case MONO_MARSHAL_COPY: {
			mono_mb_emit_ldarg (mb, i+1);
			if (sig->params [i]->byref) {
				/* make a local copy of the byref parameter. The real parameter
				 * will be updated after the xdomain call
				 */
				MonoClass *pclass = mono_class_from_mono_type (sig->params [i]);
				int copy_local = mono_mb_add_local (mb, &(pclass->byval_arg));
				mono_mb_emit_byte (mb, CEE_LDIND_REF);
				mono_mb_emit_stloc (mb, copy_local);
				mono_mb_emit_ldloc_addr (mb, copy_local);
			}
			break;
		}
		case MONO_MARSHAL_COPY_OUT:
		case MONO_MARSHAL_NONE:
			mono_mb_emit_ldarg (mb, i+1);
			break;
		}
	}

	/* Make the call to the invoke wrapper in the target domain */

	xdomain_method = mono_marshal_get_xappdomain_dispatch (method, marshal_types, complex_count, complex_out_count, ret_marshal_type);
	mono_marshal_emit_load_domain_method (mb, xdomain_method);
	mono_mb_emit_calli (mb, mono_method_signature (xdomain_method));

	if (copy_return)
		mono_mb_emit_stloc (mb, loc_return);

	/* Switch domain */

	mono_mb_emit_ldloc (mb, loc_old_domainid);
	mono_mb_emit_byte (mb, CEE_LDC_I4_0);
	mono_marshal_emit_switch_domain (mb);
	mono_mb_emit_byte (mb, CEE_POP);
	
	/* Restore thread domain data */
	
	mono_mb_emit_ldloc (mb, loc_context);
	mono_mb_emit_icall (mb, mono_context_set);
	
	/* if (loc_serialized_exc != null) ... */

	mono_mb_emit_ldloc (mb, loc_serialized_exc);
	pos_noex = mono_mb_emit_short_branch (mb, CEE_BRFALSE_S);

	mono_mb_emit_ldloc (mb, loc_serialized_exc);
	mono_marshal_emit_xdomain_copy_value (mb, byte_array_class);
	mono_mb_emit_managed_call (mb, method_rs_deserialize, NULL);
	mono_mb_emit_op (mb, CEE_CASTCLASS, mono_defaults.exception_class);
	mono_mb_emit_managed_call (mb, method_exc_fixexc, NULL);
	mono_mb_emit_byte (mb, CEE_THROW);
	mono_mb_patch_short_branch (mb, pos_noex);

	/* copy back non-serialized output parameters */

	j = 0;
	for (i = 0; i < sig->param_count; i++) {
		if (!sig->params [i]->byref || marshal_types [i] != MONO_MARSHAL_COPY) continue;
		mono_mb_emit_ldarg (mb, i + 1);
		mono_mb_emit_ldloc (mb, copy_locals_base + (j++));
		mono_marshal_emit_xdomain_copy_value (mb, mono_class_from_mono_type (sig->params [i]));
		mono_mb_emit_byte (mb, CEE_STIND_REF);
	}

	/* Deserialize out parameters */

	if (complex_out_count > 0) {
		mono_mb_emit_ldloc (mb, loc_serialized_data);
		mono_marshal_emit_xdomain_copy_value (mb, byte_array_class);
		mono_mb_emit_managed_call (mb, method_rs_deserialize, NULL);
		mono_mb_emit_stloc (mb, loc_array);
	
		/* Copy back output parameters and return type */
		
		j = 0;
		for (i = 0; i < sig->param_count; i++) {
			if (marshal_types [i] != MONO_MARSHAL_SERIALIZE) continue;
			if (sig->params[i]->byref) {
				MonoClass *pclass = mono_class_from_mono_type (sig->params [i]);
				mono_mb_emit_ldarg (mb, i + 1);
				mono_mb_emit_ldloc (mb, loc_array);
				mono_mb_emit_icon (mb, j);
				mono_mb_emit_byte (mb, CEE_LDELEM_REF);
				if (pclass->valuetype) {
					mono_mb_emit_op (mb, CEE_UNBOX, pclass);
					mono_mb_emit_op (mb, CEE_LDOBJ, pclass);
					mono_mb_emit_op (mb, CEE_STOBJ, pclass);
				} else {
					if (pclass != mono_defaults.object_class)
						mono_mb_emit_op (mb, CEE_CASTCLASS, pclass);
					mono_mb_emit_byte (mb, CEE_STIND_REF);
				}
			}
			j++;
		}
	
		if (ret_marshal_type == MONO_MARSHAL_SERIALIZE) {
			mono_mb_emit_ldloc (mb, loc_array);
			mono_mb_emit_icon (mb, complex_count);
			mono_mb_emit_byte (mb, CEE_LDELEM_REF);
			if (ret_class->valuetype) {
				mono_mb_emit_op (mb, CEE_UNBOX, ret_class);
				mono_mb_emit_op (mb, CEE_LDOBJ, ret_class);
			}
		}
	} else if (ret_marshal_type == MONO_MARSHAL_SERIALIZE) {
		mono_mb_emit_ldloc (mb, loc_serialized_data);
		mono_marshal_emit_xdomain_copy_value (mb, byte_array_class);
		mono_mb_emit_managed_call (mb, method_rs_deserialize, NULL);
		if (ret_class->valuetype) {
			mono_mb_emit_op (mb, CEE_UNBOX, ret_class);
			mono_mb_emit_op (mb, CEE_LDOBJ, ret_class);
		} else if (ret_class != mono_defaults.object_class) {
			mono_mb_emit_op (mb, CEE_CASTCLASS, ret_class);
		}
	} else {
		mono_mb_emit_ldloc (mb, loc_serialized_data);
		mono_mb_emit_byte (mb, CEE_DUP);
		pos = mono_mb_emit_short_branch (mb, CEE_BRFALSE_S);
		mono_marshal_emit_xdomain_copy_value (mb, byte_array_class);
	
		mono_mb_patch_short_branch (mb, pos);
		mono_mb_emit_managed_call (mb, method_rs_deserialize, NULL);
		mono_mb_emit_byte (mb, CEE_POP);
	}

	if (copy_return) {
		mono_mb_emit_ldloc (mb, loc_return);
		if (ret_marshal_type == MONO_MARSHAL_COPY)
			mono_marshal_emit_xdomain_copy_value (mb, ret_class);
	}

	mono_mb_emit_byte (mb, CEE_RET);
#endif /* DISABLE_JIT */

	res = mono_remoting_mb_create_and_cache (method, mb, sig, sig->param_count + 16);
	mono_mb_free (mb);

	return res;
}

MonoMethod *
mono_marshal_get_remoting_invoke_for_target (MonoMethod *method, MonoRemotingTarget target_type)
{
	if (target_type == MONO_REMOTING_TARGET_APPDOMAIN) {
		return mono_marshal_get_xappdomain_invoke (method);
	} else if (target_type == MONO_REMOTING_TARGET_COMINTEROP) {
#ifndef DISABLE_COM
		return mono_cominterop_get_invoke (method);
#else
		g_assert_not_reached ();
#endif
	} else {
		return mono_marshal_get_remoting_invoke (method);
	}
	/* Not erached */
	return NULL;
}

G_GNUC_UNUSED static gpointer
mono_marshal_load_remoting_wrapper (MonoRealProxy *rp, MonoMethod *method)
{
	if (rp->target_domain_id != -1)
		return mono_compile_method (mono_marshal_get_xappdomain_invoke (method));
	else
		return mono_compile_method (mono_marshal_get_remoting_invoke (method));
}

MonoMethod *
mono_marshal_get_remoting_invoke_with_check (MonoMethod *method)
{
	MonoMethodSignature *sig;
	MonoMethodBuilder *mb;
	MonoMethod *res, *native;
	int i, pos, pos_rem;

	g_assert (method);

	if (method->wrapper_type == MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK)
		return method;

	/* we cant remote methods without this pointer */
	g_assert (mono_method_signature (method)->hasthis);

	if ((res = mono_marshal_remoting_find_in_cache (method, MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK)))
		return res;

	sig = mono_signature_no_pinvoke (method);
	
	mb = mono_mb_new (method->klass, method->name, MONO_WRAPPER_REMOTING_INVOKE_WITH_CHECK);

#ifndef DISABLE_JIT
	for (i = 0; i <= sig->param_count; i++)
		mono_mb_emit_ldarg (mb, i);
	
	mono_mb_emit_ldarg (mb, 0);
	pos = mono_mb_emit_proxy_check (mb, CEE_BNE_UN);

	if (mono_marshal_supports_fast_xdomain (method)) {
		mono_mb_emit_ldarg (mb, 0);
		pos_rem = mono_mb_emit_xdomain_check (mb, CEE_BEQ);
		
		/* wrapper for cross app domain calls */
		native = mono_marshal_get_xappdomain_invoke (method);
		mono_mb_emit_managed_call (mb, native, mono_method_signature (native));
		mono_mb_emit_byte (mb, CEE_RET);
		
		mono_mb_patch_branch (mb, pos_rem);
	}
	/* wrapper for normal remote calls */
	native = mono_marshal_get_remoting_invoke (method);
	mono_mb_emit_managed_call (mb, native, mono_method_signature (native));
	mono_mb_emit_byte (mb, CEE_RET);

	/* not a proxy */
	mono_mb_patch_branch (mb, pos);
	mono_mb_emit_managed_call (mb, method, mono_method_signature (method));
	mono_mb_emit_byte (mb, CEE_RET);
#endif

	res = mono_remoting_mb_create_and_cache (method, mb, sig, sig->param_count + 16);
	mono_mb_free (mb);

	return res;
}

/*
 * mono_marshal_get_ldfld_remote_wrapper:
 * @klass: The return type
 *
 * This method generates a wrapper for calling mono_load_remote_field_new.
 * The return type is ignored for now, as mono_load_remote_field_new () always
 * returns an object. In the future, to optimize some codepaths, we might
 * call a different function that takes a pointer to a valuetype, instead.
 */
MonoMethod *
mono_marshal_get_ldfld_remote_wrapper (MonoClass *klass)
{
	MonoMethodSignature *sig, *csig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	static MonoMethod* cached = NULL;

	mono_marshal_lock_internal ();
	if (cached) {
		mono_marshal_unlock_internal ();
		return cached;
	}
	mono_marshal_unlock_internal ();

	mb = mono_mb_new_no_dup_name (mono_defaults.object_class, "__mono_load_remote_field_new_wrapper", MONO_WRAPPER_LDFLD_REMOTE);

	mb->method->save_lmf = 1;

	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 3);
	sig->params [0] = &mono_defaults.object_class->byval_arg;
	sig->params [1] = &mono_defaults.int_class->byval_arg;
	sig->params [2] = &mono_defaults.int_class->byval_arg;
	sig->ret = &mono_defaults.object_class->byval_arg;

#ifndef DISABLE_JIT
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldarg (mb, 1);
	mono_mb_emit_ldarg (mb, 2);

	csig = mono_metadata_signature_alloc (mono_defaults.corlib, 3);
	csig->params [0] = &mono_defaults.object_class->byval_arg;
	csig->params [1] = &mono_defaults.int_class->byval_arg;
	csig->params [2] = &mono_defaults.int_class->byval_arg;
	csig->ret = &mono_defaults.object_class->byval_arg;
	csig->pinvoke = 1;

	mono_mb_emit_native_call (mb, csig, mono_load_remote_field_new);
	mono_marshal_emit_thread_interrupt_checkpoint (mb);

	mono_mb_emit_byte (mb, CEE_RET);
#endif

	mono_marshal_lock_internal ();
	res = cached;
	mono_marshal_unlock_internal ();
	if (!res) {
		MonoMethod *newm;
		newm = mono_mb_create (mb, sig, 4, NULL);
		mono_marshal_lock_internal ();
		res = cached;
		if (!res) {
			res = newm;
			cached = res;
			mono_marshal_unlock_internal ();
		} else {
			mono_marshal_unlock_internal ();
			mono_free_method (newm);
		}
	}
	mono_mb_free (mb);

	return res;
}

/*
 * mono_marshal_get_ldfld_wrapper:
 * @type: the type of the field
 *
 * This method generates a function which can be use to load a field with type
 * @type from an object. The generated function has the following signature:
 * <@type> ldfld_wrapper (MonoObject *this, MonoClass *class, MonoClassField *field, int offset)
 */
MonoMethod *
mono_marshal_get_ldfld_wrapper (MonoType *type)
{
	MonoMethodSignature *sig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	MonoClass *klass;
	GHashTable *cache;
	WrapperInfo *info;
	char *name;
	int t, pos0, pos1 = 0;

	type = mono_type_get_underlying_type (type);

	t = type->type;

	if (!type->byref) {
		if (type->type == MONO_TYPE_SZARRAY) {
			klass = mono_defaults.array_class;
		} else if (type->type == MONO_TYPE_VALUETYPE) {
			klass = type->data.klass;
		} else if (t == MONO_TYPE_OBJECT || t == MONO_TYPE_CLASS || t == MONO_TYPE_STRING) {
			klass = mono_defaults.object_class;
		} else if (t == MONO_TYPE_PTR || t == MONO_TYPE_FNPTR) {
			klass = mono_defaults.int_class;
		} else if (t == MONO_TYPE_GENERICINST) {
			if (mono_type_generic_inst_is_valuetype (type))
				klass = mono_class_from_mono_type (type);
			else
				klass = mono_defaults.object_class;
		} else {
			klass = mono_class_from_mono_type (type);			
		}
	} else {
		klass = mono_defaults.int_class;
	}

	cache = get_cache (&klass->image->ldfld_wrapper_cache, mono_aligned_addr_hash, NULL);
	if ((res = mono_marshal_find_in_cache (cache, klass)))
		return res;

	/* we add the %p pointer value of klass because class names are not unique */
	name = g_strdup_printf ("__ldfld_wrapper_%p_%s.%s", klass, klass->name_space, klass->name); 
	mb = mono_mb_new (mono_defaults.object_class, name, MONO_WRAPPER_LDFLD);
	g_free (name);

	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 4);
	sig->params [0] = &mono_defaults.object_class->byval_arg;
	sig->params [1] = &mono_defaults.int_class->byval_arg;
	sig->params [2] = &mono_defaults.int_class->byval_arg;
	sig->params [3] = &mono_defaults.int_class->byval_arg;
	sig->ret = &klass->byval_arg;

#ifndef DISABLE_JIT
	mono_mb_emit_ldarg (mb, 0);
	pos0 = mono_mb_emit_proxy_check (mb, CEE_BNE_UN);

	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldarg (mb, 1);
	mono_mb_emit_ldarg (mb, 2);

	mono_mb_emit_managed_call (mb, mono_marshal_get_ldfld_remote_wrapper (klass), NULL);

	/*
	csig = mono_metadata_signature_alloc (mono_defaults.corlib, 3);
	csig->params [0] = &mono_defaults.object_class->byval_arg;
	csig->params [1] = &mono_defaults.int_class->byval_arg;
	csig->params [2] = &mono_defaults.int_class->byval_arg;
	csig->ret = &klass->this_arg;
	csig->pinvoke = 1;

	mono_mb_emit_native_call (mb, csig, mono_load_remote_field_new);
	mono_marshal_emit_thread_interrupt_checkpoint (mb);
	*/

	if (klass->valuetype) {
		mono_mb_emit_op (mb, CEE_UNBOX, klass);
		pos1 = mono_mb_emit_branch (mb, CEE_BR);
	} else {
		mono_mb_emit_byte (mb, CEE_RET);
	}

	mono_mb_patch_branch (mb, pos0);

	mono_mb_emit_ldarg (mb, 0);
        mono_mb_emit_byte (mb, MONO_CUSTOM_PREFIX);
        mono_mb_emit_byte (mb, CEE_MONO_OBJADDR);
	mono_mb_emit_ldarg (mb, 3);
	mono_mb_emit_byte (mb, CEE_ADD);

	if (klass->valuetype)
		mono_mb_patch_branch (mb, pos1);

	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_STRING:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		mono_mb_emit_byte (mb, mono_type_to_ldind (type));
		break;
	case MONO_TYPE_VALUETYPE:
		g_assert (!klass->enumtype);
		mono_mb_emit_op (mb, CEE_LDOBJ, klass);
		break;
	case MONO_TYPE_GENERICINST:
		if (mono_type_generic_inst_is_valuetype (type)) {
			mono_mb_emit_op (mb, CEE_LDOBJ, klass);
		} else {
			mono_mb_emit_byte (mb, CEE_LDIND_REF);
		}
		break;
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		mono_mb_emit_op (mb, CEE_LDOBJ, klass);
		break;
	default:
		g_warning ("type %x not implemented", type->type);
		g_assert_not_reached ();
	}

	mono_mb_emit_byte (mb, CEE_RET);
#endif /* DISABLE_JIT */

	info = mono_wrapper_info_create (mb, WRAPPER_SUBTYPE_NONE);
	info->d.proxy.klass = klass;
	res = mono_mb_create_and_cache_full (cache, klass,
										 mb, sig, sig->param_count + 16, info, NULL);
	mono_mb_free (mb);
	
	return res;
}

/*
 * mono_marshal_get_ldflda_wrapper:
 * @type: the type of the field
 *
 * This method generates a function which can be used to load a field address
 * from an object. The generated function has the following signature:
 * gpointer ldflda_wrapper (MonoObject *this, MonoClass *class, MonoClassField *field, int offset);
 */
MonoMethod *
mono_marshal_get_ldflda_wrapper (MonoType *type)
{
	MonoMethodSignature *sig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	MonoClass *klass;
	GHashTable *cache;
	WrapperInfo *info;
	char *name;
	int t, pos0, pos1, pos2, pos3;

	type = mono_type_get_underlying_type (type);
	t = type->type;

	if (!type->byref) {
		if (type->type == MONO_TYPE_SZARRAY) {
			klass = mono_defaults.array_class;
		} else if (type->type == MONO_TYPE_VALUETYPE) {
			klass = type->data.klass;
		} else if (t == MONO_TYPE_OBJECT || t == MONO_TYPE_CLASS || t == MONO_TYPE_STRING ||
			   t == MONO_TYPE_CLASS) { 
			klass = mono_defaults.object_class;
		} else if (t == MONO_TYPE_PTR || t == MONO_TYPE_FNPTR) {
			klass = mono_defaults.int_class;
		} else if (t == MONO_TYPE_GENERICINST) {
			if (mono_type_generic_inst_is_valuetype (type))
				klass = mono_class_from_mono_type (type);
			else
				klass = mono_defaults.object_class;
		} else {
			klass = mono_class_from_mono_type (type);			
		}
	} else {
		klass = mono_defaults.int_class;
	}

	cache = get_cache (&klass->image->ldflda_wrapper_cache, mono_aligned_addr_hash, NULL);
	if ((res = mono_marshal_find_in_cache (cache, klass)))
		return res;

	/* we add the %p pointer value of klass because class names are not unique */
	name = g_strdup_printf ("__ldflda_wrapper_%p_%s.%s", klass, klass->name_space, klass->name); 
	mb = mono_mb_new (mono_defaults.object_class, name, MONO_WRAPPER_LDFLDA);
	g_free (name);

	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 4);
	sig->params [0] = &mono_defaults.object_class->byval_arg;
	sig->params [1] = &mono_defaults.int_class->byval_arg;
	sig->params [2] = &mono_defaults.int_class->byval_arg;
	sig->params [3] = &mono_defaults.int_class->byval_arg;
	sig->ret = &mono_defaults.int_class->byval_arg;

#ifndef DISABLE_JIT
	/* if typeof (this) != transparent_proxy goto pos0 */
	mono_mb_emit_ldarg (mb, 0);
	pos0 = mono_mb_emit_proxy_check (mb, CEE_BNE_UN);

	/* if same_appdomain goto pos1 */
	mono_mb_emit_ldarg (mb, 0);
	pos1 = mono_mb_emit_xdomain_check (mb, CEE_BEQ);

	mono_mb_emit_exception_full (mb, "System", "InvalidOperationException", "Attempt to load field address from object in another appdomain.");

	/* same app domain */
	mono_mb_patch_branch (mb, pos1);

	/* if typeof (this) != contextbound goto pos2 */
	mono_mb_emit_ldarg (mb, 0);
	pos2 = mono_mb_emit_contextbound_check (mb, CEE_BEQ);

	/* if this->rp->context == mono_context_get goto pos3 */
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoTransparentProxy, rp));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoRealProxy, context));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_icall (mb, mono_context_get);
	pos3 = mono_mb_emit_branch (mb, CEE_BEQ);

	mono_mb_emit_exception_full (mb, "System", "InvalidOperationException", "Attempt to load field address from object in another context.");

	mono_mb_patch_branch (mb, pos2);
	mono_mb_patch_branch (mb, pos3);

	/* return the address of the field from this->rp->unwrapped_server */
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoTransparentProxy, rp));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoRealProxy, unwrapped_server));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	mono_mb_emit_byte (mb, MONO_CUSTOM_PREFIX);
	mono_mb_emit_byte (mb, CEE_MONO_OBJADDR);
	mono_mb_emit_ldarg (mb, 3);
	mono_mb_emit_byte (mb, CEE_ADD);
	mono_mb_emit_byte (mb, CEE_RET);

	/* not a proxy: return the address of the field directly */
	mono_mb_patch_branch (mb, pos0);

	mono_mb_emit_ldarg (mb, 0);
        mono_mb_emit_byte (mb, MONO_CUSTOM_PREFIX);
        mono_mb_emit_byte (mb, CEE_MONO_OBJADDR);
	mono_mb_emit_ldarg (mb, 3);
	mono_mb_emit_byte (mb, CEE_ADD);

	mono_mb_emit_byte (mb, CEE_RET);
#endif

	info = mono_wrapper_info_create (mb, WRAPPER_SUBTYPE_NONE);
	info->d.proxy.klass = klass;
	res = mono_mb_create_and_cache_full (cache, klass,
										 mb, sig, sig->param_count + 16,
										 info, NULL);
	mono_mb_free (mb);
	
	return res;
}

/*
 * mono_marshal_get_stfld_remote_wrapper:
 * klass: The type of the field
 *
 *  This function generates a wrapper for calling mono_store_remote_field_new
 * with the appropriate signature.
 * Similarly to mono_marshal_get_ldfld_remote_wrapper () this doesn't depend on the
 * klass argument anymore.
 */
MonoMethod *
mono_marshal_get_stfld_remote_wrapper (MonoClass *klass)
{
	MonoMethodSignature *sig, *csig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	static MonoMethod *cached = NULL;

	mono_marshal_lock_internal ();
	if (cached) {
		mono_marshal_unlock_internal ();
		return cached;
	}
	mono_marshal_unlock_internal ();

	mb = mono_mb_new_no_dup_name (mono_defaults.object_class, "__mono_store_remote_field_new_wrapper", MONO_WRAPPER_STFLD_REMOTE);

	mb->method->save_lmf = 1;

	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 4);
	sig->params [0] = &mono_defaults.object_class->byval_arg;
	sig->params [1] = &mono_defaults.int_class->byval_arg;
	sig->params [2] = &mono_defaults.int_class->byval_arg;
	sig->params [3] = &mono_defaults.object_class->byval_arg;
	sig->ret = &mono_defaults.void_class->byval_arg;

#ifndef DISABLE_JIT
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldarg (mb, 1);
	mono_mb_emit_ldarg (mb, 2);
	mono_mb_emit_ldarg (mb, 3);

	csig = mono_metadata_signature_alloc (mono_defaults.corlib, 4);
	csig->params [0] = &mono_defaults.object_class->byval_arg;
	csig->params [1] = &mono_defaults.int_class->byval_arg;
	csig->params [2] = &mono_defaults.int_class->byval_arg;
	csig->params [3] = &mono_defaults.object_class->byval_arg;
	csig->ret = &mono_defaults.void_class->byval_arg;
	csig->pinvoke = 1;

	mono_mb_emit_native_call (mb, csig, mono_store_remote_field_new);
	mono_marshal_emit_thread_interrupt_checkpoint (mb);

	mono_mb_emit_byte (mb, CEE_RET);
#endif

	mono_marshal_lock_internal ();
	res = cached;
	mono_marshal_unlock_internal ();
	if (!res) {
		MonoMethod *newm;
		newm = mono_mb_create (mb, sig, 6, NULL);
		mono_marshal_lock_internal ();
		res = cached;
		if (!res) {
			res = newm;
			cached = res;
			mono_marshal_unlock_internal ();
		} else {
			mono_marshal_unlock_internal ();
			mono_free_method (newm);
		}
	}
	mono_mb_free (mb);
	
	return res;
}

/*
 * mono_marshal_get_stfld_wrapper:
 * @type: the type of the field
 *
 * This method generates a function which can be use to store a field with type
 * @type. The generated function has the following signature:
 * void stfld_wrapper (MonoObject *this, MonoClass *class, MonoClassField *field, int offset, <@type> val)
 */
MonoMethod *
mono_marshal_get_stfld_wrapper (MonoType *type)
{
	MonoMethodSignature *sig;
	MonoMethodBuilder *mb;
	MonoMethod *res;
	MonoClass *klass;
	GHashTable *cache;
	WrapperInfo *info;
	char *name;
	int t, pos;

	type = mono_type_get_underlying_type (type);
	t = type->type;

	if (!type->byref) {
		if (type->type == MONO_TYPE_SZARRAY) {
			klass = mono_defaults.array_class;
		} else if (type->type == MONO_TYPE_VALUETYPE) {
			klass = type->data.klass;
		} else if (t == MONO_TYPE_OBJECT || t == MONO_TYPE_CLASS || t == MONO_TYPE_STRING) {
			klass = mono_defaults.object_class;
		} else if (t == MONO_TYPE_PTR || t == MONO_TYPE_FNPTR) {
			klass = mono_defaults.int_class;
		} else if (t == MONO_TYPE_GENERICINST) {
			if (mono_type_generic_inst_is_valuetype (type))
				klass = mono_class_from_mono_type (type);
			else
				klass = mono_defaults.object_class;
		} else {
			klass = mono_class_from_mono_type (type);			
		}
	} else {
		klass = mono_defaults.int_class;
	}

	cache = get_cache (&klass->image->stfld_wrapper_cache, mono_aligned_addr_hash, NULL);
	if ((res = mono_marshal_find_in_cache (cache, klass)))
		return res;

	/* we add the %p pointer value of klass because class names are not unique */
	name = g_strdup_printf ("__stfld_wrapper_%p_%s.%s", klass, klass->name_space, klass->name); 
	mb = mono_mb_new (mono_defaults.object_class, name, MONO_WRAPPER_STFLD);
	g_free (name);

	sig = mono_metadata_signature_alloc (mono_defaults.corlib, 5);
	sig->params [0] = &mono_defaults.object_class->byval_arg;
	sig->params [1] = &mono_defaults.int_class->byval_arg;
	sig->params [2] = &mono_defaults.int_class->byval_arg;
	sig->params [3] = &mono_defaults.int_class->byval_arg;
	sig->params [4] = &klass->byval_arg;
	sig->ret = &mono_defaults.void_class->byval_arg;

#ifndef DISABLE_JIT
	mono_mb_emit_ldarg (mb, 0);
	pos = mono_mb_emit_proxy_check (mb, CEE_BNE_UN);

	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldarg (mb, 1);
	mono_mb_emit_ldarg (mb, 2);
	mono_mb_emit_ldarg (mb, 4);
	if (klass->valuetype)
		mono_mb_emit_op (mb, CEE_BOX, klass);

	mono_mb_emit_managed_call (mb, mono_marshal_get_stfld_remote_wrapper (klass), NULL);

	mono_mb_emit_byte (mb, CEE_RET);

	mono_mb_patch_branch (mb, pos);

	mono_mb_emit_ldarg (mb, 0);
        mono_mb_emit_byte (mb, MONO_CUSTOM_PREFIX);
        mono_mb_emit_byte (mb, CEE_MONO_OBJADDR);
	mono_mb_emit_ldarg (mb, 3);
	mono_mb_emit_byte (mb, CEE_ADD);
	mono_mb_emit_ldarg (mb, 4);

	switch (t) {
	case MONO_TYPE_I1:
	case MONO_TYPE_U1:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_I2:
	case MONO_TYPE_U2:
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_STRING:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		mono_mb_emit_byte (mb, mono_type_to_stind (type));
		break;
	case MONO_TYPE_VALUETYPE:
		g_assert (!klass->enumtype);
		mono_mb_emit_op (mb, CEE_STOBJ, klass);
		break;
	case MONO_TYPE_GENERICINST:
	case MONO_TYPE_VAR:
	case MONO_TYPE_MVAR:
		mono_mb_emit_op (mb, CEE_STOBJ, klass);
		break;
	default:
		g_warning ("type %x not implemented", type->type);
		g_assert_not_reached ();
	}

	mono_mb_emit_byte (mb, CEE_RET);
#endif

	info = mono_wrapper_info_create (mb, WRAPPER_SUBTYPE_NONE);
	info->d.proxy.klass = klass;
	res = mono_mb_create_and_cache_full (cache, klass,
										 mb, sig, sig->param_count + 16,
										 info, NULL);
	mono_mb_free (mb);
	
	return res;
}

MonoMethod *
mono_marshal_get_proxy_cancast (MonoClass *klass)
{
	static MonoMethodSignature *isint_sig = NULL;
	GHashTable *cache;
	MonoMethod *res;
	WrapperInfo *info;
	int pos_failed, pos_end;
	char *name, *klass_name;
	MonoMethod *can_cast_to;
	MonoMethodDesc *desc;
	MonoMethodBuilder *mb;

	cache = get_cache (&klass->image->proxy_isinst_cache, mono_aligned_addr_hash, NULL);
	if ((res = mono_marshal_find_in_cache (cache, klass)))
		return res;

	if (!isint_sig) {
		isint_sig = mono_metadata_signature_alloc (mono_defaults.corlib, 1);
		isint_sig->params [0] = &mono_defaults.object_class->byval_arg;
		isint_sig->ret = &mono_defaults.object_class->byval_arg;
		isint_sig->pinvoke = 0;
	}

	klass_name = mono_type_full_name (&klass->byval_arg);
	name = g_strdup_printf ("__proxy_isinst_wrapper_%s", klass_name); 
	mb = mono_mb_new (mono_defaults.object_class, name, MONO_WRAPPER_PROXY_ISINST);
	g_free (klass_name);
	g_free (name);
	
	mb->method->save_lmf = 1;

#ifndef DISABLE_JIT
	/* get the real proxy from the transparent proxy*/
	mono_mb_emit_ldarg (mb, 0);
	mono_mb_emit_ldflda (mb, MONO_STRUCT_OFFSET (MonoTransparentProxy, rp));
	mono_mb_emit_byte (mb, CEE_LDIND_REF);
	
	/* get the reflection type from the type handle */
	mono_mb_emit_ptr (mb, &klass->byval_arg);
	mono_mb_emit_icall (mb, type_from_handle);
	
	mono_mb_emit_ldarg (mb, 0);
	
	/* make the call to CanCastTo (type, ob) */
	desc = mono_method_desc_new ("IRemotingTypeInfo:CanCastTo", FALSE);
	can_cast_to = mono_method_desc_search_in_class (desc, mono_defaults.iremotingtypeinfo_class);
	g_assert (can_cast_to);
	mono_method_desc_free (desc);
	mono_mb_emit_op (mb, CEE_CALLVIRT, can_cast_to);
	
	pos_failed = mono_mb_emit_branch (mb, CEE_BRFALSE);

	/* Upgrade the proxy vtable by calling: mono_upgrade_remote_class_wrapper (type, ob)*/
	mono_mb_emit_ptr (mb, &klass->byval_arg);
	mono_mb_emit_icall (mb, type_from_handle);
	mono_mb_emit_ldarg (mb, 0);
	
	mono_mb_emit_icall (mb, mono_upgrade_remote_class_wrapper);
	mono_marshal_emit_thread_interrupt_checkpoint (mb);
	
	mono_mb_emit_ldarg (mb, 0);
	pos_end = mono_mb_emit_branch (mb, CEE_BR);
	
	/* fail */
	
	mono_mb_patch_branch (mb, pos_failed);
	mono_mb_emit_byte (mb, CEE_LDNULL);
	
	/* the end */
	
	mono_mb_patch_branch (mb, pos_end);
	mono_mb_emit_byte (mb, CEE_RET);
#endif

	info = mono_wrapper_info_create (mb, WRAPPER_SUBTYPE_NONE);
	info->d.proxy.klass = klass;
	res = mono_mb_create_and_cache_full (cache, klass, mb, isint_sig, isint_sig->param_count + 16, info, NULL);
	mono_mb_free (mb);

	return res;
}

void
mono_upgrade_remote_class_wrapper (MonoReflectionType *rtype, MonoTransparentProxy *tproxy)
{
	MonoClass *klass;
	MonoDomain *domain = ((MonoObject*)tproxy)->vtable->domain;
	klass = mono_class_from_mono_type (rtype->type);
	mono_upgrade_remote_class (domain, (MonoObject*)tproxy, klass);
}

#else /* DISABLE_REMOTING */

void
mono_remoting_init (void)
{
}

#endif /* DISABLE_REMOTING */

/* mono_get_xdomain_marshal_type()
 * Returns the kind of marshalling that a type needs for cross domain calls.
 */
static MonoXDomainMarshalType
mono_get_xdomain_marshal_type (MonoType *t)
{
	switch (t->type) {
	case MONO_TYPE_VOID:
		g_assert_not_reached ();
		break;
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		return MONO_MARSHAL_NONE;
	case MONO_TYPE_STRING:
		return MONO_MARSHAL_COPY;
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY: {
		MonoClass *elem_class = mono_class_from_mono_type (t)->element_class;
		if (mono_get_xdomain_marshal_type (&(elem_class->byval_arg)) != MONO_MARSHAL_SERIALIZE)
			return MONO_MARSHAL_COPY;
		break;
	}
	default:
		break;
	}
	return MONO_MARSHAL_SERIALIZE;
}

/* mono_marshal_xdomain_copy_value
 * Makes a copy of "val" suitable for the current domain.
 */
MonoObject *
mono_marshal_xdomain_copy_value (MonoObject *val)
{
	MonoDomain *domain;
	if (val == NULL) return NULL;

	domain = mono_domain_get ();

	switch (mono_object_class (val)->byval_arg.type) {
	case MONO_TYPE_VOID:
		g_assert_not_reached ();
		break;
	case MONO_TYPE_U1:
	case MONO_TYPE_I1:
	case MONO_TYPE_BOOLEAN:
	case MONO_TYPE_U2:
	case MONO_TYPE_I2:
	case MONO_TYPE_CHAR:
	case MONO_TYPE_U4:
	case MONO_TYPE_I4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_R4:
	case MONO_TYPE_R8: {
		return mono_value_box (domain, mono_object_class (val), ((char*)val) + sizeof(MonoObject));
	}
	case MONO_TYPE_STRING: {
		MonoString *str = (MonoString *) val;
		return (MonoObject *) mono_string_new_utf16 (domain, mono_string_chars (str), mono_string_length (str));
	}
	case MONO_TYPE_ARRAY:
	case MONO_TYPE_SZARRAY: {
		MonoArray *acopy;
		MonoXDomainMarshalType mt = mono_get_xdomain_marshal_type (&(mono_object_class (val)->element_class->byval_arg));
		if (mt == MONO_MARSHAL_SERIALIZE) return NULL;
		acopy = mono_array_clone_in_domain (domain, (MonoArray *) val);
		if (mt == MONO_MARSHAL_COPY) {
			int i, len = mono_array_length (acopy);
			for (i = 0; i < len; i++) {
				MonoObject *item = mono_array_get (acopy, gpointer, i);
				mono_array_setref (acopy, i, mono_marshal_xdomain_copy_value (item));
			}
		}
		return (MonoObject *) acopy;
	}
	default:
		break;
	}

	return NULL;
}
