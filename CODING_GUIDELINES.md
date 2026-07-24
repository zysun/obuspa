# OB-USP-Agent Coding Guidelines

Please follow these guidelines to ensure code consistency.

## Function Headers
All functions should have function header comment blocks describing the function, input and output arguments and return values.

```c
GOOD:

/*********************************************************************//**
**
** TEXT_UTILS_StrncpyLen
**
** Performs a strncpy() of a source string whose length is specified (rather than being NULL terminated)
**
** \param   dst - pointer to buffer to copy source string into
** \param   dst_len - length of the destination buffer
** \param   src - pointer to buffer containing string to copy
** \param   src_len - length of the source buffer
**
** \return  None
**
**************************************************************************/
void TEXT_UTILS_StrncpyLen(char *dst, int dst_len, char *src, int src_len)
{
}
```

## Indentation
Code blocks should be indented by 4 spaces. Do not use tabs.

## Braces
Use Allman brace style (which means braces are on separate lines from code), and always use braces even for single line statements. Do not try to remove braces or put everything on one line. NOTE: You can put everything on one line if it's a #define though (see #define coding guidelines section), but still use braces even for #defines containing single statements.  
Cases of switch statements shouldn't have braces unless it's needed because you're declaring a variable specific to a particular case.

```c
BAD:
if (len <=0) return NULL;
if (len <=0)  { return NULL; }
if (len <=0) {
    return NULL;
}
#define log_stuff(level, str)  if (level >= verbosity)  print(str);

GOOD:
if (len<=0)
{
    return NULL;
}
#define log_stuff(level, str)  if (level >= verbosity) { print(str); }
```

## Variable naming convention
Use lower case for variable names and underscores to separate words in the name.

```c
BAD:
IsValid
myArray

GOOD:
is_valid
my_array
```

## OBUSPA Function Naming convention
Capitalize the first letter of all words in the function name. Only use underscores like a full stop or colon in the 'sentence' making up the function name, rather than between every individual word. Typically this is when you have many functions all starting with the same words, but a different ending. e.g. API functions.

```c
BAD:
validateValue()
validate_value()

GOOD: 
ValidateValue()
GetValues_CaseA()
GetValues_CaseB()
```

## API Functions
Functions that are called from other C files (API functions) should be at the top of the C file and should be named using the name of the file (or a shortened form of it), as a prefix all in capitals, with words separated by underscores.  
e.g. In stomp.c  STOMP_EnableConnection()

  
## Order of functions in the file
Apart from API functions which should always be at the top of the file, the general rule for ordering of functions is high level to low level. This means that you will often need to declare function prototypes near the start of the file, as dependencies will be at the bottom of the file, whilst the callers of the function will be at the top of the C file. This is not a hard and fast rule, but generally higher level code (in call stack terms) should be at the top of the file and lower level code at the bottom.  
Also order code in terms of lifecyle order. For example Add() before Delete() and  ProcessConnecting() before ProcessConnected().  
Finally, group together all functions with the same stem. For example put all NotifyChange_XXX() functions together and all Validate_XXX() functions together in the file.  
NOTE: The order of function prototype declarations near the top of the file does not matter. It's purely there to keep the C compiler happy.

  
## Comments
Typically each code block/guard clause should have a comment before it stating the intent of the code in human readable terms.  
Use C++ style comments, within a function i.e. //  
Do not put really long multi-line comments in the middle of a function. It should be possible to summarize in 2 or (max) 3 comment lines what the code is doing. If you need to say more than this, then put the comment in the function header.  
If you write code to handle some special case which is not obvious that it could occur (e.g. because the reviewer might expect that the case was already handled somewhere else) or it's not obvious why it would occur, then please make sure that the conditions under which the case occurs are included in the comment.

Do not reference any bug numbers in comments (because the BBF Jira system is not visible to people accessing the code from GitHub). Change logs and release notes are the place to reference GitHub tickets.

  
## Defensive coding
Don't write code that contains if tests for states which should never occur if the software is working correctly. Instead use asserts (USP_ASSERT macro) for that.  
Writing code that tries to cope with states that the software should never get into, shows a lack of understanding of the software and unnecessarily complicates the code. Also, when reviewing, it makes the reviewer wonder if that state could occur and how it would occur.

That said, API functions for a software module should guard against incorrect use of the API and hence are likely to contain defensive coding cases.

## #defines
Avoid long and complex #defines. Avoid use of the line continuation character ie. try to put the whole of the #define definition on a single line.  Use function calls instead if the define really needs to be split over multiple lines.

## State Changes
Avoid partial states. If some event occurs that affects more than one variable, ensure that all variables are updated at the same time. Do not try to update some at one time, and the rest at another.  
In particular be consistent with lifetimes. Have only one variable in a structure which controls whether the structure is in use or not, and do not reference other variables in the structure if the structure is marked as not in use.

## Switch statements
Don't use nested if statements, when you should be using a switch statement.  
Switch statements should include all cases explicitly and a default case. Do not leave any cases out - this demonstrates to the reviewer that you actually considered all cases, and makes it easier to review, because the reviewer doesn't have to then find the enumerated type and mentally check off all of the other cases are actually the default.

```c
BAD:
if (x == kEnum1)
{
}
else if (x==kEnum2)
{
}
else
{
}

GOOD:
switch(x)
{
    case kEnum1:
        break;
    case kEnum2:
        break;
    default:
    case kEnum3:
    case kEnum4:
        break;
}
```

## Use of negation operator and treating non booleans as booleans in if statements
Avoid the use of the negation operator in conditionals. Instead use explicit values, true or false or scalar value.

```c
BAD:
if (!strcmp(x,y))
if (!is_correct)
if (!payload_len)

GOOD:
if (strcmp(x,y)==0)
if (is_correct==false)
if (payload_len==0)
```

NOTE: It's OK for testing against true to omit the '==true', if the name of the variable is obviously a boolean. Some variable names would still be better with the explicit '==true' though.

```c
OK:
if (is_enabled)
```

Also avoid treating pointers or scalars as booleans in if statements. Instead test against explicit value.

```c
BAD:
if (ptr)
if (strcmp(x,y))
if (*first_char)

GOOD:
if (ptr != NULL)
if (strcmp(x,y) != 0)
if (*first_char != '\0')
```

## Explicit return values for errors
When returning an error value directly from a conditional, if this is a known fixed value, return that value, rather than returning a variable which you know to have that value. This applies to the case when returning from a conditional. Of course when returning from a single exit point in a function, this does not apply.

```c
BAD:
if (ptr==NULL)
{
    return ptr;
}

GOOD:
if (ptr==NULL)
{
    return NULL;
}
```

## Nested code blocks
Avoid nesting code blocks. Always try to keep the code as flat as possible.  
For example, most code consists of various tests for special/simpler/error cases, followed by code that does the main purpose of the function. Always use un-nested conditionals for the special/error cases containing a return or goto exit (if a single exit point for the function is desirable to unwind resources taken during the course of the function).   
Avoid the use of else-if. It should be rare that an 'else if' is required - usually the inclusion of one means that the if/elseif statement chain should be in a separate function, rewritten to avoid the 'else-if' by making the first 'if' return if it fires.  
Use 'goto exit' instead of 'return' in the conditionals if you need a single exit point to unwind resources taken during the course of the function.  
NOTE: For each if statement there should be comment on the line before it. The examples omit this for brevity.

```c
BAD:
if (error1==false)
{ 
    if (error2==false)
    {
        //  main body
    }
    else
    {
        // handle error 2
    }
}
else
{
    // handle error 1
}

BAD:
if (error1)
{
    // handle error 1
}
elseif (error 2)
{
    // handle error 2
}
else
{
    // main body
}

GOOD:
if (error1)
{
    // handle error 1
    return;
}

if (error2)
{
    // handle error 2
    return;
}

// main body
return;
```

## Order of variables in conditionals
Always put the variable which the function is considering or varies most first, and the one that is more constant last

```c
BAD:
if (expected != received)
if (expiry_time < time_now)
if (expiry_time - time_now < 0)
if (match==index)  // e.g. in a for loop using index

GOOD:
if (received != expected)
if (time_now >= expiry_time)
if (index==match)  // e.g. in a for loop using index
```

## Boolean Algebra
In general, avoid boolean algebra. Instead use conditionals. If you do use boolean algebra, then assign the result to a suitably named boolean variable.  
Using conditionals is easier to understand as you only have to understand the 'true' or the 'false' case at a single time rather than having to understand both in the same statement. It also allows you to comment the case much more easily.

```c
BAD:
return ((x==val1) && (y==val2));

GOOD:
// Comment for case in if statement
if ((x==val1) && (y==val2))
{
    return true;
}
return false;
```

  
## Variable Declarations
Variable declarations should be at the top of the function (classic C style). There's no need to initialise them explicitly unless you're wanting to set a default.  
Typically put all declarations at the start of the function (classic C style).  
Do not use C++ style where the declaration and assignment are done on the same line and the declaration is delayed until the point it is needed.  
But it's OK to put declarations after a brace at the start of a code block, if the declaration is only used within that code block. Typically this is mostly used if the code block is conditionally compiled into the code.

  
## Statements
Statements should obviously do only one 'thing', don't try to do more than one 'thing' in a single statement.  
For example:

- Don't do assignment within an if statement.

- Avoid increment within an assignment.

- Avoid calling a function and checking it's return code within an if statement (Major exception: strcmp - which is only doing a comparison, so allowed)

```c
BAD:
if ((err=func()) != 0)
{
}

GOOD:
err = func()
if (err != 0)
{
}

---------
BAD:
if (func(param1, param2, param3, param4) != SUCCESS)
{
}

GOOD:
err = func(param1, param2, param3, param4);
if (err != SUCCESS)
{
}

---------
BAD:
x = (y++)*8;

GOOD:
x = y*8;
y++;
```

## Pre or post increment
Always use post increment operator. Avoid pre-increment operator.  
Avoid use of this operator in assignment or conditional statements.

## For statements
Use for statements only for the classic case of loop index, using a single variable. Use while, or do-while for all other cases.  
Avoid using '\<=' for the conditional. Always try to use '\<'.  
Do not perform increment/decrement on more than one loop index (eg incrementing a pointer as well as a loop index).  
Do not declare the loop index variable within the for statement.

## Pointer Arithmetic
Avoid direct pointer arithmetic. Instead use the address-of '&' operator.

```c
BAD:
ptr = base + offset;

GOOD:
ptr = &base[offset];
```

## Accessing Arrays of structures in Loops
Create a variable to point to the current item under consideration. Name the ptr using a short name (3 or 4 characters).

```c
BAD: 
for (i=0; i<NUM_ELEM(my_array); i++)
{
    if ((my_array[i].field1 == val1) || (my_array[i].field2 ==val2))
    {
    }
}

GOOD: 
for (i=0; i<NUM_ELEM(my_array); i++)
{
    cur = &my_array[i];
    if ((cur->field1 == val1) || (cur->field2 ==val2))
    {

    }
}
```

## Dead Code
Don't write code that isn't 'plumbed' into the rest of the code because you don't plan to support that feature. In particular, don't fill out the data model with lots of parameters, that then aren't tied into controlling/reporting because the lower level code doesn't support it.   
If there is any dead code, don't just enclose it in '#if 0' but comment it out also (using C++ style comments).  
It's OK for short (usually one line) statements to be commented out, in order that they could be uncommented by a developer to gain extra debug etc.

## Use the simplest solution that works now
Always use the simplest solution to the set of features which you're adding now. Don't write code thinking that 'in the future it might need to handle XXX' so I'm going to make it much more complex now with that in mind (but still not actually handle XXX).

## Nested Structures
In general, avoid structures defined in terms of other structures. Avoid unnecessary nesting, keep structures as flat as possible.

  
## Avoid ptr-to-ptr return types
Sometimes this cannot be avoided, but often it can. Return the ptr directly, rather than as an output argument. For example a function that needs to return a buffer and the length of the buffer

```c
BAD:
int GetBuf(void **buf)   // returns len, -1 indicates error

GOOD:
void *GetBuf(int *len)   // returns buf, NULL indicates error
```

## Unsigned vs Signed types
In general avoid unsigned types, especially if you are going to perform calculations mixing the unsigned type with signed types. Only use unsigned types if you really need the extra bit for maximum value, or the type is never involved in calculations mixing signed and unsigned types.  
Also avoid overthinking the size of variables (uint8 vs uint16 vs uint32), unless you really need to save space.

## Initialisation of structures
Always memset to 0, followed by explicit initialisation of ALL fields (even if those fields are 0 or NULL).  
Do not use inline initializers eg mystruct x = {field1_val, field2_val, ...}

## Brackets in Conditionals
Put brackets around each condition in a conditional, even if it is not strictly necessary.

```c
BAD:
if (x==val1 && y==val2)
if (IsMatch(x) && x->field1==val1)

GOOD:
if ((x==val1) && (y==val2))
if ((IsMatch(x))  && (x->field1==val1))
```

## Logging statements
All errors, warnings and error logging statements must be prefixed by the name of the function in which they occurred.

```c
BAD:
USP_ERR_SetMessage("Stuff happened");
USP_LOG_Error("Stuff happened");

GOOD:
USP_ERR_SetMessage("%s: Stuff happened", __FUNCTION__);
USP_LOG_Error("%s: Stuff happened", __FUNCTION__);
```

Positive informational logging statements don't have to be prefixed but it is usually advisable.

Avoid using generic error messages. Error messages should be as specific as possible.

```c
BAD:
USP_ERR_SetMessage("%s: Invalid path %s", __FUNCTION__, path);

GOOD:
USP_ERR_SetMessage("%s: Instance numbers invalid in path %s", __FUNCTION__, path);
```

## Dereferencing
Avoid using dereferencing on return parameters. Instead introduce a stack variable and only set the output parameter once. This makes the code more readable, because it isn't constantly dereferencing the output arg. The dereferencing is visual clutter.

```c
BAD:
bool get_index(char *s1, char *c, int *index)
{
    *index = 0; 
    while (s1[*index] != '\0')
    {
        if (s1[*index] == c)
        {
            return true;
        }
        (*index)++;
    }
    return false;
}

GOOD:
bool get_index(char *s1, char *c, int *index)
{
    int i;

    i = 0;
    while (s1[i] != '\0')
    {
        if (s1[i] == c)
        {
            *index = i;
            return true;
        }
        i++;
    }
    return false;
}
```
