#include <gargantua.h>
void work(void)
{
    $transaction
    {
        $transaction
        {
            $log("ok");
        }
        return;
    }
}
