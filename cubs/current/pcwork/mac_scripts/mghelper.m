/*
 * mghelper: stand-in for /usr/libexec/MobileGestaltHelper (JOB_PROGRAM in mkjobs.py points the launchd job at it; the kernel marks
 * it a platform binary). The real helper never answers during iOS first boot on this port and lsd waits ~16 minutes for it.
 * The service is an NSXPC one: -[ getServerAnswerForQuestion:reply: ] with a reply block taking an NSDictionary. This one logs every
 * question to stderr (= /dev/console) and answers at once. The shape of the reply (a dictionary with the key "answer"?) is not known;
 * every ask of the same question gets the next of a list of shapes, and the log says which, so the shape after which the asking
 * stops is the one that works.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

@protocol MGHelperProtocol
- (void)getServerAnswerForQuestion:(id)question reply:(void (^)(NSDictionary *))reply;
@end

@interface MGHelperImpl : NSObject <MGHelperProtocol>
@end

@implementation MGHelperImpl
- (void)getServerAnswerForQuestion:(id)question reply:(void (^)(NSDictionary *))reply
{
	static int seen;
	typedef id (*server_fn)(id, id, int *, id *);
	static server_fn answer_fn;
	typedef id (*local_fn)(id, id, int *);
	static local_fn local_answer;
	static dispatch_once_t once;

	dispatch_once(&once, ^{
		void *h = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_NOW);
		void (*setserver)(int) = h ? dlsym(h, "_MGSetServer") : NULL;

		answer_fn = h ? dlsym(h, "_MGServerCopyAnswerWithError") : NULL;
		local_answer = h ? dlsym(h, "MGCopyAnswerWithError") : NULL;
		if (setserver) {
			setserver(1);
		}
		fprintf(stderr, "mghelper: MGSetServer %p MGServerCopyAnswerWithError %p\n", setserver, answer_fn);
	});
	NSString *q = [question isKindOfClass:[NSString class]] ? question : @"?";
	int err = 0;
	id errs = nil, a = nil;

	fprintf(stderr, "mghelper: question #%d: %s\n", ++seen, [q UTF8String]);
	if (local_answer) {
		a = local_answer(q, nil, &err);
	} else if (answer_fn) {
		a = answer_fn(((id (*)(id, SEL))objc_msgSend)([NSXPCConnection class], sel_registerName("currentConnection")), q, &err, &errs);
	}
	fprintf(stderr, "mghelper:   answer %s err %d [%s]\n", a ? [[a description] UTF8String] : "(nil)", err,
	    errs ? [[errs description] UTF8String] : "");
	reply(a ? @{ @"answer" : a } : @{});
}
@end

@interface MGHelperDelegate : NSObject <NSXPCListenerDelegate>
@end

@implementation MGHelperDelegate
- (BOOL)listener:(NSXPCListener *)listener shouldAcceptNewConnection:(NSXPCConnection *)c
{
	c.exportedInterface = [NSXPCInterface interfaceWithProtocol:@protocol(MGHelperProtocol)];
	c.exportedObject = [MGHelperImpl new];
	[c resume];
	return YES;
}
@end

static void
on_abort(int sig)
{
	void *bt[40];
	int n = backtrace(bt, 40);

	dprintf(2, "mghelper: signal %d, backtrace of %d frames\n", sig, n);
	backtrace_symbols_fd(bt, n, 2);
	_exit(134);
}

int
main(void)
{
	@autoreleasepool {
		setvbuf(stderr, NULL, _IOLBF, 0);
		signal(SIGABRT, on_abort);
		signal(SIGILL, on_abort);
		signal(SIGTRAP, on_abort);
		fprintf(stderr, "mghelper: started pid %d\n", getpid());
		NSXPCListener *l = ((id (*)(id, SEL, id))objc_msgSend)([NSXPCListener alloc], sel_registerName("initWithMachServiceName:"),
		    @"com.apple.mobilegestalt.xpc");

		l.delegate = [MGHelperDelegate new];
		[l resume];
		[[NSRunLoop currentRunLoop] run];
	}
	return 0;
}
