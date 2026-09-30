from pathlib import Path

OUT = Path(__file__).with_name('golden_e2e.tsv')
rows=[]

def esc(v):
    return '[' + v.replace('\\','\\\\').replace(']','\\]') + ']'

def exp(status='OK', resolved='-', argc=0, args='', calls=0):
    return f'status={status}|resolved={esc(resolved)}|argc={argc}|args={esc(args)}|handler_calls={calls}'

def add(i, scope, mode, inp, expected):
    rows.append((i, scope, mode, inp, expected))

def ok(i, cmd, args=()):
    add(i,'BASE','EXEC',cmd + ((' ' + ' '.join(args)) if args else ''),exp('OK',cmd,len(args),'|'.join(args),1))

# Direct command matrix: arity boundaries and normal paths.
ok('direct-status','status')
for a in [(),('main',),('a','b'),('a','b','c')]:
    if not a:
        add('direct-echo-0','BASE','EXEC','echo',exp('OK','echo',0,'',1))
    else:
        add('direct-echo-'+str(len(a)),'BASE','EXEC','echo '+' '.join(a),exp('OK','echo',len(a),'|'.join(a),1))
for n,args in [(0,()),(1,('main',))]:
    add(f'direct-remote-{n}','BASE','EXEC','remote'+((' '+' '.join(args)) if args else ''),exp('OK','remote',n,'|'.join(args),1))
add('direct-remote-add','BASE','EXEC','remote add origin git@example.com',exp('OK','remote add',2,'origin|git@example.com',1))
add('direct-remote-get','BASE','EXEC','remote get origin',exp('OK','remote get',1,'origin',1))
add('direct-checkout','BASE','EXEC','checkout main',exp('OK','checkout',1,'main',1))
add('direct-build-0','BASE','EXEC','build',exp('OK','build',0,'',1))
add('direct-build-1','BASE','EXEC','build debug',exp('OK','build',1,'debug',1))
add('direct-build-2','BASE','EXEC','build debug linux',exp('OK','build',2,'debug|linux',1))
add('direct-config-get','BASE','EXEC','config get',exp('OK','config get',0,'',1))
add('direct-config-set','BASE','EXEC','config set theme dark',exp('OK','config set',2,'theme|dark',1))
add('direct-fail','BASE','EXEC','fail',exp('HANDLER_ERROR','fail',0,'',1))

# Arity/error matrix.
add('arity-checkout-0','BASE','EXEC','checkout',exp('INVALID_ARGUMENT','-',0,'',0))
add('arity-checkout-2','BASE','EXEC','checkout a b',exp('INVALID_ARGUMENT','-',0,'',0))
add('arity-remote-add-0','BASE','EXEC','remote add',exp('INVALID_ARGUMENT','-',0,'',0))
add('arity-remote-add-1','BASE','EXEC','remote add origin',exp('INVALID_ARGUMENT','-',0,'',0))
add('arity-remote-add-3','BASE','EXEC','remote add origin git@example.com extra',exp('INVALID_ARGUMENT','-',0,'',0))
add('arity-echo-4','BASE','EXEC','echo a b c d',exp('INVALID_ARGUMENT','-',0,'',0))
add('arity-build-3','BASE','EXEC','build a b c',exp('INVALID_ARGUMENT','-',0,'',0))
add('unknown-command','BASE','EXEC','does-not-exist',exp('NOT_FOUND','-',0,'',0))
add('unknown-subcommand','BASE','EXEC','remote delete origin',exp('INVALID_ARGUMENT','-',0,'',0))
add('empty-command','BASE','EXEC','',exp('PARSE_ERROR','-',0,'',0))
add('whitespace-only','BASE','EXEC','   ',exp('PARSE_ERROR','-',0,'',0))
add('parse-unterminated-quote','BASE','EXEC','echo "unterminated',exp('PARSE_ERROR','-',0,'',0))
add('parse-trailing-escape','BASE','EXEC','echo trailing\\',exp('PARSE_ERROR','-',0,'',0))

# Longest-prefix resolution.
add('longest-remote-add','BASE','EXEC','remote add origin url',exp('OK','remote add',2,'origin|url',1))
add('longest-remote-get','BASE','EXEC','remote get origin',exp('OK','remote get',1,'origin',1))
add('longest-config-set','BASE','EXEC','config set key value',exp('OK','config set',2,'key|value',1))
add('longest-config-get','BASE','EXEC','config get',exp('OK','config get',0,'',1))

# Alias positional substitution matrix.
add('alias-st','BASE','EXEC','st',exp('OK','status',0,'',1))
for x in ['main','feature/x','hot-fix']:
    add('alias-co-'+x,'BASE','EXEC','co '+x,exp('OK','checkout',1,x,1))
add('alias-co-missing','BASE','EXEC','co',exp('PARSE_ERROR','-',0,'',0))
add('alias-co-extra','BASE','EXEC','co main extra',exp('OK','checkout',1,'main',1))
add('alias-ra','BASE','EXEC','ra origin url',exp('OK','remote add',2,'origin|url',1))
add('alias-ra-missing','BASE','EXEC','ra origin',exp('PARSE_ERROR','-',0,'',0))
add('alias-ra-extra','BASE','EXEC','ra origin url extra',exp('OK','remote add',2,'origin|url',1))
for args in [(),('hello',),('hello','world'),('a','b','c')]:
    inp='say'+((' '+' '.join(args)) if args else '')
    add('alias-say-'+str(len(args)),'BASE','EXEC',inp,exp('OK','echo',len(args),'|'.join(args),1))
add('alias-say-over-max','BASE','EXEC','say a b c d',exp('INVALID_ARGUMENT','-',0,'',0))
add('alias-say1','BASE','EXEC','say1 hello',exp('OK','echo',1,'hello',1))
add('alias-say1-missing','BASE','EXEC','say1',exp('PARSE_ERROR','-',0,'',0))
add('alias-cfg','BASE','EXEC','cfg',exp('OK','config get',0,'',1))
add('alias-cfgset','BASE','EXEC','cfgset key value',exp('OK','config set',2,'key|value',1))
add('alias-cfgset-missing','BASE','EXEC','cfgset key',exp('PARSE_ERROR','-',0,'',0))
add('alias-fail','BASE','EXEC','failalias',exp('HANDLER_ERROR','fail',0,'',1))
add('alias-cycle','BASE','EXEC','cycle-a',exp('ALIAS_CYCLE','-',0,'',0))

# Tokenization/quoting/escaping.
add('quote-checkout-double','BASE','EXEC','checkout "feature branch"',exp('OK','checkout',1,'feature branch',1))
add('quote-checkout-escape','BASE','EXEC','checkout feature\\ branch',exp('OK','checkout',1,'feature branch',1))
add('quote-remote-add','BASE','EXEC','remote add "origin one" "ssh://host/repo"',exp('OK','remote add',2,'origin one|ssh://host/repo',1))
add('quote-echo-empty','BASE','EXEC','echo ""',exp('OK','echo',1,'',1))
add('quote-echo-spaces','BASE','EXEC','echo "hello world" test',exp('OK','echo',2,'hello world|test',1))
add('quote-echo-escaped-quote','BASE','EXEC','echo "a \\"quoted\\" value"',exp('OK','echo',1,'a "quoted" value',1))
add('quote-echo-backslash','BASE','EXEC','echo path\\\\file',exp('OK','echo',1,'path\\file',1))
add('quote-single-literal','BASE','EXEC',"echo 'hello world'",exp('OK','echo',2,"'hello|world'",1))

# Binding matrix.
add('bind-status','BASE','BIND','CTRL+SHIFT:P',exp('OK','status',0,'',1))
add('bind-build','BASE','BIND','CTRL+SHIFT:B',exp('OK','build',0,'',1))
add('bind-alias','BASE','BIND','CTRL+SHIFT:C',exp('OK','checkout',1,'main',1))
add('bind-alt','BASE','BIND','ALT:X',exp('OK','checkout',1,'main',1))
add('bind-shift','BASE','BIND','SHIFT:E',exp('OK','echo',2,'bound|hello',1))
add('bind-wrong-modifier','BASE','BIND','CTRL:P',exp('NOT_FOUND','-',0,'',0))
add('bind-wrong-key','BASE','BIND','CTRL+SHIFT:Z',exp('NOT_FOUND','-',0,'',0))
add('bind-none','BASE','BIND','NONE:P',exp('NOT_FOUND','-',0,'',0))

# Persisted aliases and bindings. The persisted config intentionally uses ALT as default modifier.
for ident,inp,expected in [
    ('pco','pco persisted',exp('OK','checkout',1,'persisted',1)),
    ('pra','pra origin persisted-url',exp('OK','remote add',2,'origin|persisted-url',1)),
    ('psay','psay one two',exp('OK','echo',2,'one|two',1)),
    ('pst','pst',exp('OK','status',0,'',1)),
]:
    add('persist-'+ident,'PERSIST','EXEC',inp,expected)
add('persist-bind-status','PERSIST','BIND','ALT:P',exp('OK','status',0,'',1))
add('persist-bind-alias','PERSIST','BIND','ALT:C',exp('OK','checkout',1,'persisted',1))
add('persist-default-alt','PERSIST','DEFAULT','P',exp('OK','status',0,'',1))
add('persist-default-old-mod','PERSIST','BIND','CTRL+SHIFT:P',exp('OK','status',0,'',1))

# Same commands with punctuation, digits, and repeated whitespace.
add('whitespace-between','BASE','EXEC','remote   add    origin    url',exp('OK','remote add',2,'origin|url',1))
add('whitespace-leading','BASE','EXEC','   checkout main',exp('OK','checkout',1,'main',1))
add('whitespace-trailing','BASE','EXEC','status   ',exp('OK','status',0,'',1))
add('args-numeric','BASE','EXEC','echo 1 22 333',exp('OK','echo',3,'1|22|333',1))
add('args-punctuation','BASE','EXEC','echo a-b c_d x.y',exp('OK','echo',3,'a-b|c_d|x.y',1))

# Add deterministic permutations of common two-argument commands.
vals1=['origin','upstream','o']
vals2=['url','ssh://host/repo','git@example.com']
for i,a in enumerate(vals1):
    for j,b in enumerate(vals2):
        add(f'perm-ra-{i}-{j}','BASE','EXEC',f'ra {a} {b}',exp('OK','remote add',2,f'{a}|{b}',1))
        add(f'perm-direct-ra-{i}-{j}','BASE','EXEC',f'remote add {a} {b}',exp('OK','remote add',2,f'{a}|{b}',1))

# Alias * substitution with quoted token permutations.
add('perm-say-quoted','BASE','EXEC','say "alpha beta" gamma',exp('OK','echo',2,'alpha beta|gamma',1))
add('perm-say-three','BASE','EXEC','say alpha "beta gamma" delta',exp('OK','echo',3,'alpha|beta gamma|delta',1))

# Persisted invalid/wrong bindings and unknown alias.
add('persist-unknown','PERSIST','EXEC','does-not-exist',exp('NOT_FOUND','-',0,'',0))
add('persist-wrong-binding','PERSIST','BIND','CTRL+SHIFT:Z',exp('NOT_FOUND','-',0,'',0))
add('roundtrip-pco','ROUNDTRIP','EXEC','pco persisted',exp('OK','checkout',1,'persisted',1))
add('roundtrip-pra','ROUNDTRIP','EXEC','pra origin persisted-url',exp('OK','remote add',2,'origin|persisted-url',1))
add('roundtrip-default','ROUNDTRIP','DEFAULT','P',exp('OK','status',0,'',1))

with OUT.open('w', encoding='utf-8') as f:
    f.write('# id<TAB>scope<TAB>mode<TAB>input<TAB>expected\n')
    f.write('# Expected is a canonical, exact oracle record. Do not regenerate this file in CI.\n')
    for row in rows:
        f.write('\t'.join(row) + '\n')
print(f'wrote {len(rows)} cases to {OUT}')
