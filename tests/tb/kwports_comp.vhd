-- Component declaration for kwports.v (tests/test_regress.py)
library ieee;
use ieee.std_logic_1164.all;

package kwports_comp is
    component kwports
        port (
            \next\ : in std_logic;
            \In\ : in std_logic;
            \in_\ : in std_logic_vector(3 downto 0);
            \out\ : out std_logic_vector(3 downto 0);
            both : out std_logic);
    end component;
end package;
