#include "unity.h"
#include <stdbool.h>
#include <stdlib.h>
#include "../../examples/autotest-validate/autotest-validate.h"
#include "../../assignment-autotest/test/assignment1/username-from-conf-file.h"

/**
* This function should:
*   1) Call the my_username() function in Test_assignment_validate.c to get your hard coded username.
*   2) Obtain the value returned from function malloc_username_from_conf_file() in username-from-conf-file.h within
*       the assignment autotest submodule at assignment-autotest/test/assignment1/
*   3) Use unity assertion TEST_ASSERT_EQUAL_STRING_MESSAGE the two strings are equal.  See
*       the [unity assertion reference](https://github.com/ThrowTheSwitch/Unity/blob/master/docs/UnityAssertionsReference.md)
*/
void test_validate_my_username()
{
	// Implementation for testing validation of username.
	// First we will store our "expected" username, then we will store the conf username
	// Then we will compare our stored locals and see if they match
	// Bonus ToDo: would be to add sanity checking for not nulls on the locals before compare

	// Step 1) Get our hard coded username from autotest-validate.c [my_username()]
	const char *expected_username = my_username();
	// Step 2) Grab our username read from the /conf/username.txt using malloc_username_from_conf_file() helper
	char *from_conf_username = malloc_username_from_conf_file();
	
	// Step 3) Test if our usernames match!
	TEST_ASSERT_EQUAL_STRING_MESSAGE(expected_username, from_conf_username, "Usernames DO NOT Match!!");

	// Clean up our malloc'd string so we free that memory
	free(from_conf_username);
}
