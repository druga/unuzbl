% SWI Prolog

:- use_module(library(arithmetic)).
% http://www.complang.tuwien.ac.at/ulrich/Prolog-inedit/lambda.pl
%:- use_module(lambda).

:- arithmetic_function(binary_number/1).
:- arithmetic_function(binary_number/2).
:- arithmetic_function(my_copysign/2).
:- arithmetic_function(my_sign/1).
:- arithmetic_function(immediate_word/4).
:- arithmetic_function(flatten_diff/1).
:- arithmetic_function(parse/1).
:- arithmetic_function(optimize/1).
:- arithmetic_function(evaluate/1).
:- arithmetic_function(storing/1).

my_copysign(X,Y,Z) :-
	Z is copysign(X,Y).
my_sign(X,Y) :-
	Y is sign(X).
erlang_writef(F,A) :-
	format(F,A).
my_recordz(K,P) :-
	% like P0 =.. [K,P], assertz(P0)
	recordz(K,P).
parse(Fn,AST) :-
	call_semidet(phrase_from_file(sentence(AST),Fn,[buffer_size(16384)])).
call_nth(Goal,C) :-
	State = count(0,_), Goal, arg(1,State,C1), 
	C2 is C1 + 1, nb_setarg(1,State,C2), C = C2.
